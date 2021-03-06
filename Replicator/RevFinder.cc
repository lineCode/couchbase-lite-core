//
// RevFinder.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "RevFinder.hh"
#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "IncomingRev.hh"
#include "DBAccess.hh"
#include "Increment.hh"
#include "StringUtil.hh"
#include "Instrumentation.hh"
#include "c4.hh"
#include "c4Transaction.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "BLIP.hh"
#include "fleece/Fleece.hh"

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    RevFinder::RevFinder(Replicator *replicator, Delegate *delegate)
    :Worker(replicator, "RevFinder")
    ,_delegate(delegate)
    {
        _passive = _options.pull <= kC4Passive;
        registerHandler("changes",          &RevFinder::handleChanges);
        registerHandler("proposeChanges",   &RevFinder::handleChanges);
    }


    // Receiving an incoming "changes" (or "proposeChanges") message
    void RevFinder::handleChanges(Retained<MessageIn> req) {
        if (pullerHasCapacity()) {
            handleChangesNow(req);
        } else {
            logVerbose("Queued '%.*s' REQ#%" PRIu64 " (now %zu)",
                       SPLAT(req->property("Profile"_sl)), req->number(),
                       _waitingChangesMessages.size() + 1);
            Signpost::begin(Signpost::handlingChanges, (uintptr_t)req->number());
            _waitingChangesMessages.push_back(move(req));
        }
    }


    void RevFinder::_reRequestingRev() {
        increment(_numRevsBeingRequested);
    }


    void RevFinder::_revReceived() {
        decrement(_numRevsBeingRequested);

        // Process waiting "changes" messages if not throttled:
        while (!_waitingChangesMessages.empty() && pullerHasCapacity()) {
            auto req = _waitingChangesMessages.front();
            _waitingChangesMessages.pop_front();
            handleChangesNow(req);
        }
    }


    // Actually handle a "changes" message:
    void RevFinder::handleChangesNow(MessageIn *req) {
        slice reqType = req->property("Profile"_sl);
        bool proposed = (reqType == "proposeChanges"_sl);
        logVerbose("Handling '%.*s' REQ#%" PRIu64, SPLAT(reqType), req->number());

        auto changes = req->JSONBody().asArray();
        auto nChanges = changes.count();
        if (!changes && req->body() != "null"_sl) {
            warn("Invalid body of 'changes' message");
            req->respondWithError({"BLIP"_sl, 400, "Invalid JSON body"_sl});
        } else if (nChanges == 0) {
            // Empty array indicates we've caught up.
            logInfo("Caught up with remote changes");
            _delegate->caughtUp();
            req->respond();
        } else if (req->noReply()) {
            warn("Got pointless noreply 'changes' message");
        } else if (_options.noIncomingConflicts() && !proposed) {
            // In conflict-free mode the protocol requires the pusher send "proposeChanges" instead
            req->respondWithError({"BLIP"_sl, 409});
            
        } else {
            // Alright, let's look at the changes:
            if (proposed) {
                logInfo("Received %u changes", nChanges);
            } else if (willLog()) {
                alloc_slice firstSeq(changes[0].asArray()[0].toString());
                alloc_slice lastSeq (changes[nChanges-1].asArray()[0].toString());
                logInfo("Received %u changes (seq '%.*s'..'%.*s')",
                        nChanges, SPLAT(firstSeq), SPLAT(lastSeq));
            }

            if (!proposed)
                _db->markRevsSyncedNow();   // make sure foreign ancestors are up to date

            MessageBuilder response(req);
            response.compressed = true;
            _db->use([&](C4Database *db) {
                response["maxHistory"_sl] = c4db_getMaxRevTreeDepth(db);
            });
            if (!_db->disableBlobSupport())
                response["blobs"_sl] = "true"_sl;
            if ( !_announcedDeltaSupport && !_options.disableDeltaSupport()) {
                response["deltas"_sl] = "true"_sl;
                _announcedDeltaSupport = true;
            }

            Stopwatch st;

            vector<ChangeSequence> sequences(nChanges); // the vector I will send to the delegate

            auto &encoder = response.jsonBody();
            encoder.beginArray();
            unsigned requested = proposed ? findProposedRevs(changes, encoder, sequences)
                                          : findRevs(changes, encoder, sequences);
            encoder.endArray();
            
            // CBL-1399: Important that the order be call expectSequences and *then* respond
            // to avoid rev messages comes in before the Puller knows about them (mostly 
            // applies to local to local replication where things can come back over the wire
            // very quickly)
            _numRevsBeingRequested += requested;
            _delegate->expectSequences(move(sequences));
            req->respond(response);

            logInfo("Responded to '%.*s' REQ#%" PRIu64 " w/request for %u revs in %.6f sec",
                    SPLAT(req->property("Profile"_sl)), req->number(), requested, st.elapsed());

        }

        Signpost::end(Signpost::handlingChanges, (uintptr_t)req->number());
    }


    // Looks through the contents of a "changes" message, encodes the response,
    // adds each entry to `sequences`, and returns the number of new revs.
    unsigned RevFinder::findRevs(Array changes,
                                 Encoder &encoder,
                                 vector<ChangeSequence> &sequences)
    {
        // Compile the docIDs/revIDs into parallel vectors:
        unsigned itemsWritten = 0, requested = 0;
        vector<slice> docIDs, revIDs;
        auto nChanges = changes.count();
        docIDs.reserve(nChanges);
        revIDs.reserve(nChanges);
        size_t i = 0;
        for (auto item : changes) {
            // "changes" entry: [sequence, docID, revID, deleted?, bodySize?]
            auto change = item.asArray();
            docIDs.push_back(change[1].asString());
            revIDs.push_back(change[2].asString());
            sequences[i].sequence = RemoteSequence(change[0]);
            sequences[i].bodySize = max(change[4].asUnsigned(), (uint64_t)1);
            ++i;
        }

        // Ask the database to look up the ancestors:
        vector<C4StringResult> ancestors(nChanges);
        C4Error err;
        bool ok = _db->use<bool>([&](C4Database *db) {
            return c4db_findDocAncestors(db, nChanges, kMaxPossibleAncestors,
                                         !_options.disableDeltaSupport(),  // requireBodies
                                         _db->remoteDBID(),
                                         (C4String*)docIDs.data(), (C4String*)revIDs.data(),
                                         ancestors.data(), &err);
        });
        if (!ok) {
            gotError(err);
        } else {
            // Look through the database response:
            for (i = 0; i < nChanges; ++i) {
                alloc_slice docID(docIDs[i]);
                alloc_slice revID(revIDs[i]);
                alloc_slice anc(std::move(ancestors[i]));
                if (anc == kC4AncestorExistsButNotCurrent) {
                    // This means the rev exists but is not marked as the latest from the
                    // remote server, so I better make it so:
                    logDebug("    - Already have '%.*s' %.*s but need to mark it as remote ancestor",
                             SPLAT(docID), SPLAT(revID));
                    _db->setDocRemoteAncestor(docID, revID);
                    replicator()->docRemoteAncestorChanged(docID, revID);
                    sequences[i].bodySize = 0; // don't want the rev
                } else if (anc == kC4AncestorExists) {
                    sequences[i].bodySize = 0; // don't want the rev
                } else {
                    // Don't have revision -- request it:
                    ++requested;
                    // Append zeros for any items I skipped, using only writeRaw to avoid confusing
                    // the JSONEncoder's comma mechanism (CBL-1208).
                    if (itemsWritten > 0)
                        encoder.writeRaw(",");      // comma after previous array item
                    while (itemsWritten++ < i)
                        encoder.writeRaw("0,");
                    // Append array of ancestor revs I do have (it's already a JSON array):
                    encoder.writeRaw(anc ? slice(anc) : "[]"_sl);
                    logDebug("    - Requesting '%.*s' %.*s, ancestors %.*s",
                             SPLAT(docID), SPLAT(revID), SPLAT(anc));
                }
            }
        }
        return requested;
    }


    // Same as `findOrRequestRevs`, but for "proposeChanges" messages.
    unsigned RevFinder::findProposedRevs(Array changes,
                                         Encoder &encoder,
                                         vector<ChangeSequence> &sequences)
    {
        unsigned itemsWritten = 0, requested = 0;
        int i = -1;
        for (auto item : changes) {
            ++i;
            // Look up each revision in the `req` list:
            // "proposeChanges" entry: [docID, revID, parentRevID?, bodySize?]
            auto change = item.asArray();
            alloc_slice docID( change[0].asString() );
            slice revID = change[1].asString();
            if (docID.size == 0 || revID.size == 0) {
                warn("Invalid entry in 'changes' message");
                continue;     // ???  Should this abort the replication?
            }

            slice parentRevID = change[2].asString();
            if (parentRevID.size == 0)
                parentRevID = nullslice;
            alloc_slice currentRevID;
            int status = findProposedChange(docID, revID, parentRevID, currentRevID);
            if (status == 0) {
                // Accept rev by (lazily) appending a 0:
                logDebug("    - Accepting proposed change '%.*s' #%.*s with parent %.*s",
                         SPLAT(docID), SPLAT(revID), SPLAT(parentRevID));
                ++requested;
                Assert(sequences[i].bodySize == 0);
                sequences[i].bodySize = max(change[3].asUnsigned(), (uint64_t)1);
                // sequences[i].sequence remains null: proposeChanges entries have no sequence ID
            } else {
                // Reject rev by appending status code:
                logInfo("Rejecting proposed change '%.*s' #%.*s with parent %.*s (status %d; current rev is %.*s)",
                        SPLAT(docID), SPLAT(revID), SPLAT(parentRevID), status, SPLAT(currentRevID));
                while (itemsWritten++ < i)
                    encoder.writeInt(0);
                encoder.writeInt(status);
            }
        }
        return requested;
    }


    // Checks whether the revID (if any) is really current for the given doc.
    // Returns an HTTP-ish status code: 0=OK, 409=conflict, 500=internal error
    int RevFinder::findProposedChange(slice docID, slice revID, slice parentRevID,
                                     alloc_slice &outCurrentRevID)
    {
        C4Error err;
        //OPT: We don't need the document body, just its metadata, but there's no way to say that
        c4::ref<C4Document> doc = _db->getDoc(docID, &err);
        if (!doc) {
            if (isNotFoundError(err)) {
                // Doc doesn't exist; it's a conflict if the peer thinks it does:
                return parentRevID ? 409 : 0;
            } else {
                gotError(err);
                return 500;
            }
        }
        int status;
        if (slice(doc->revID) == revID) {
            // I already have this revision:
            status = 304;
        } else if (!parentRevID) {
            // Peer is creating new doc; that's OK if doc is currently deleted:
            status = (doc->flags & kDocDeleted) ? 0 : 409;
        } else if (slice(doc->revID) != parentRevID) {
            // Peer's revID isn't current, so this is a conflict:
            status = 409;
        } else {
            // I don't have this revision and it's not a conflict, so I want it!
            status = 0;
        }
        if (status > 0)
            outCurrentRevID = slice(doc->revID);
        return status;
    }


} }
