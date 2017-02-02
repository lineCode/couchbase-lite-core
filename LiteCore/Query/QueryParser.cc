//
//  QueryParser.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "QueryParser.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "Path.hh"
#include "Logging.hh"
#include <utility>
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore {


#pragma mark - UTILITY FUNCTIONS:


    [[noreturn]] __printflike(1, 2)
    static void fail(const char *format, ...) {
        va_list args;
        va_start(args, format);
        char *cmessage;
        vasprintf(&cmessage, format, args);
        va_end(args);

        Warn("Invalid query: %s", cmessage);
        string message{cmessage};
        free(cmessage);
        throw error(error::LiteCore, error::InvalidQuery, message);
    }

    // printf args for a slice; matching format spec should be %.*s
    #define splat(SLICE)    (int)(SLICE).size, (SLICE).buf


    static bool isAlphanumericOrUnderscore(slice str) {
        if (str.size == 0)
            return false;
        for (size_t i = 0; i < str.size; i++)
            if (!isalnum(str[i]) && str[i] != '_')
                return false;
        return true;
    }

    static bool isValidIdentifier(slice str) {
        return isAlphanumericOrUnderscore(str) && !isdigit(str[0]);
    }


    static inline std::ostream& operator<< (std::ostream& o, slice s) {
        o.write((const char*)s.buf, s.size);
        return o;
    }


    static const Array* mustBeArray(const Value *v, const char *elseMessage = "Expected a JSON array") {
        auto a = v ? v->asArray() : nullptr;
        if (!a)
            fail("%s", elseMessage);
        return a;
    }


    // Appends two property-path strings.
    static string appendPaths(const string &parent, string child) {
        if (child[0] == '$') {
            if (child[1] == '.')
                child = child.substr(2);
            else
                child = child.substr(1);
        }
        if (parent.empty())
            return child;
        else if (child[0] == '[')
            return parent + child;
        else
            return parent + "." + child;
    }


    // Writes a string with SQL quoting (inside apostrophes, doubling contained apostrophes.)
    /*static*/ void QueryParser::writeSQLString(std::ostream &out, slice str) {
        out << "'";
        bool simple = true;
        for (unsigned i = 0; i < str.size; i++) {
            if (str[i] == '\'') {
                simple = false;
                break;
            }
        }
        if (simple) {
            out.write((const char*)str.buf, str.size);
        } else {
            for (unsigned i = 0; i < str.size; i++) {
                if (str[i] == '\'')
                    out.write("''", 2);
                else
                    out.write((const char*)&str[i], 1);
            }
        }
        out << "'";
    }

    
    static string propertyFromOperands(Array::iterator &operands);
    static string propertyFromNode(const Value *node);


#pragma mark - QUERY PARSER TOP LEVEL:


    void QueryParser::reset() {
        _context.clear();
        _context.push_back(&kOuterOperation);
    }


    void QueryParser::parseJSON(slice expressionJSON) {
        alloc_slice expressionFleece = JSONConverter::convertJSON(expressionJSON);
        return parse(Value::fromTrustedData(expressionFleece));
    }
    
    
    void QueryParser::parse(const Value *expression) {
        reset();
        if (expression->asDict()) {
            // Given a dict; assume it's the operands of a SELECT:
            writeSelect(expression->asDict());
        } else {
            const Array *a = expression->asArray();
            if (a && a->count() > 0 && a->get(0)->asString() == "SELECT"_sl) {
                // Given an entire SELECT statement:
                parseNode(expression);
            } else {
                // Given some other expression; treat it as a WHERE clause of an implicit SELECT:
                writeSelect(expression, nullptr);
            }
        }
    }


    void QueryParser::parseJustExpression(const Value *expression) {
        reset();
        parseNode(expression);
    }

    
    void QueryParser::writeSelect(const Dict *operands) {
        writeSelect(operands->get("WHERE"_sl), operands);
    }


    void QueryParser::writeSelect(const Value *where, const Dict *operands) {
        // Have to find all properties involved in MATCH before emitting the FROM clause:
        if (where)
            findFTSProperties(where);

        // 'What' clause:
        _sql << "SELECT ";
        int nCol = 0;
        for (auto &col : _baseResultColumns)
            _sql << (nCol++ ? ", " : "") << col;
        for (auto ftsTable : _ftsTables) {
            _sql << (nCol++ ? ", " : "") << "offsets(\"" << ftsTable << "\")";
        }
        _1stCustomResultCol = nCol;

        auto what = operands ? operands->get("WHAT"_sl) : nullptr;
        if (what) {
            const Array *whats = what->asArray();
            if (!whats)
                fail("WHAT must be an array");
            for (Array::iterator i(whats); i; ++i) {
                if (nCol++)
                    _sql << ", ";
                writeResultColumn(i.value());
            }
        }
        if (nCol == 0)
            fail("No result columns");

        // FROM clause:
        _sql << " FROM ";
        auto from = operands ? operands->get(" FROM"_sl) : nullptr;
        if (from) {
            fail("FROM parameter to SELECT isn't supported yet, sorry");
        } else {
            _sql << _tableName;
            unsigned ftsTableNo = 0;
            for (auto ftsTable : _ftsTables) {
                _sql << ", \"" << ftsTable << "\" AS FTS" << ++ftsTableNo;
            }
        }

        // WHERE clause:
        if (where) {
            _sql << " WHERE ";
            parseNode(where);
        }

        // ORDER BY clause:
        auto order = operands ? operands->get("ORDER BY"_sl) : nullptr;
        if (order) {
            _sql << " ORDER BY ";
            _context.push_back(&kOrderByOperation); // suppress parens around arg list
            Array::iterator orderBys(mustBeArray(order));
            writeColumnList(orderBys);
            _context.pop_back();
        }

        // LIMIT, OFFSET clauses:
        // TODO: Use the ones from operands
        if (!_defaultLimit.empty())
            _sql << " LIMIT " << _defaultLimit;
        if (!_defaultOffset.empty())
            _sql << " OFFSET " << _defaultOffset;
    }


    void QueryParser::writeCreateIndex(const Array *expressions) {
        reset();
        _sql << "CREATE INDEX IF NOT EXISTS \"" << indexName(expressions) << "\" ON " << _tableName << " ";
        Array::iterator iter(expressions);
        writeColumnList(iter);
        // TODO: Add 'WHERE' clause for use with SQLite 3.15+
    }


    void QueryParser::writeResultColumn(const Value *val) {
        switch (val->type()) {
            case kArray:
                parseNode(val);
                return;
            case kString: {
                slice str = val->asString();
                if (str == "*"_sl) {
                    fail("'*' result column isn't supported");
                    return;
                } else {
                    // "."-prefixed string becomes a property
                    writeStringLiteralAsProperty(str);
                    return;
                }
                break;
            }
            default:
                break;
        }
        fail("Invalid item type in WHAT clause; must be array or '*' or '.property'");
    }


    void QueryParser::writeStringLiteralAsProperty(slice str) {
        if (str.size == 0 || str[0] != '.')
            fail("Invalid property name; must start with '.'");
        str.moveStart(1);
        writePropertyGetter("fl_value", str.asString());
    }


#pragma mark - PARSING THE "WHERE" CLAUSE:
    
    
    // This table defines the operators and their characteristics.
    // Each operator has a name, min/max argument count, precedence, and a handler method.
    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
    // http://www.sqlite.org/lang_expr.html
    typedef void (QueryParser::*OpHandler)(slice op, Array::iterator& args);
    struct QueryParser::Operation {
        slice op; int minArgs; int maxArgs; int precedence; OpHandler handler;};
    const QueryParser::Operation QueryParser::kOperationList[] = {
        {"."_sl,       1, 9,  9,  &QueryParser::propertyOp},
        {"$"_sl,       1, 1,  9,  &QueryParser::parameterOp},
        {"?"_sl,       1, 9,  9,  &QueryParser::variableOp},

        {"MISSING"_sl, 0, 0,  9,  &QueryParser::missingOp},

        {"||"_sl,      2, 9,  8,  &QueryParser::infixOp},

        {"*"_sl,       2, 9,  7,  &QueryParser::infixOp},
        {"/"_sl,       2, 2,  7,  &QueryParser::infixOp},
        {"%"_sl,       2, 2,  7,  &QueryParser::infixOp},

        {"+"_sl,       2, 9,  6,  &QueryParser::infixOp},
        {"-"_sl,       2, 2,  6,  &QueryParser::infixOp},
        {"-"_sl,       1, 1,  9,  &QueryParser::prefixOp},

        {"<"_sl,       2, 2,  4,  &QueryParser::infixOp},
        {"<="_sl,      2, 2,  4,  &QueryParser::infixOp},
        {">"_sl,       2, 2,  4,  &QueryParser::infixOp},
        {">="_sl,      2, 2,  4,  &QueryParser::infixOp},

        {"="_sl,       2, 2,  3,  &QueryParser::infixOp},
        {"!="_sl,      2, 2,  3,  &QueryParser::infixOp},
        {"IS"_sl,      2, 2,  3,  &QueryParser::infixOp},
        {"IS NOT"_sl,  2, 2,  3,  &QueryParser::infixOp},
        {"IN"_sl,      2, 9,  3,  &QueryParser::inOp},
        {"NOT IN"_sl,  2, 9,  3,  &QueryParser::inOp},
        {"LIKE"_sl,    2, 2,  3,  &QueryParser::infixOp},
        {"MATCH"_sl,   2, 2,  3,  &QueryParser::matchOp},
        {"BETWEEN"_sl, 3, 3,  3,  &QueryParser::betweenOp},
        {"EXISTS"_sl,  1, 1,  8,  &QueryParser::existsOp},

        {"NOT"_sl,     1, 1,  9,  &QueryParser::prefixOp},
        {"AND"_sl,     2, 9,  2,  &QueryParser::infixOp},
        {"OR"_sl,      2, 9,  2,  &QueryParser::infixOp},

        {"ANY"_sl,     3, 3,  1,  &QueryParser::anyEveryOp},
        {"EVERY"_sl,   3, 3,  1,  &QueryParser::anyEveryOp},
        {"ANY AND EVERY"_sl, 3, 3,  1,  &QueryParser::anyEveryOp},

        {"SELECT"_sl,  1, 1,  1,  &QueryParser::selectOp},

        {"DESC"_sl,    1, 1,  2,  &QueryParser::postfixOp},

        {nullslice,    0, 0, 10,  &QueryParser::fallbackOp} // fallback; must come last
    };

    const QueryParser::Operation QueryParser::kArgListOperation
        {","_sl,       0, 9, -2, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kColumnListOperation
        {","_sl,       0, 9, -2, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kOrderByOperation
        {"ORDER BY"_sl,1, 9, -3, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kOuterOperation
        {nullslice,    1, 1, -1};


    void QueryParser::parseNode(const Value *node) {
        switch (node->type()) {
            case kNull:
                _sql << "x''";        // Represent a Fleece/JSON/N1QL null as an empty blob (?)
                break;
            case kNumber:
                _sql << node->toString();
                break;
            case kBoolean:
                _sql << (node->asBool() ? '1' : '0');    // SQL doesn't have true/false
                break;
            case kString: {
                slice str = node->asString();
                if (_context.back() == &kColumnListOperation)
                    writeStringLiteralAsProperty(str);
                else
                    writeSQLString(str);
                break;
            }
            case kData:
                fail("Binary data not supported in query");
            case kArray:
                parseOpNode((const Array*)node);
                break;
            case kDict:
                fail("Dictionaries not supported in query");
                break;
        }
    }


    void QueryParser::parseOpNode(const Array *node) {
        Array::iterator array(node);
        if (array.count() == 0)
            fail("Empty JSON array");
        slice op = array[0]->asString();
        if (!op)
            fail("Operation must be a string");
        ++array;

        // Look up the handler:
        int nargs = min(array.count(), 9u);
        bool nameMatched = false;
        const Operation *def;
        for (def = kOperationList; def->op; ++def) {
            if (op == def->op) {
                nameMatched = true;
                if (nargs >= def->minArgs && nargs <= def->maxArgs)
                    break;
            }
        }
        if (nameMatched && !def->op)
            fail("Wrong number of arguments to %.*s", splat(op));
        handleOperation(def, op, array);
    }


    // Invokes an Operation's handler. Pushes Operation on the stack and writes parens if needed
    void QueryParser::handleOperation(const Operation* op,
                                      slice actualOperator,
                                      Array::iterator& operands)
    {
        bool parenthesize = (op->precedence <= _context.back()->precedence);
        _context.push_back(op);
        if (parenthesize)
            _sql << '(';

        auto handler = op->handler;
        (this->*handler)(actualOperator, operands);

        if (parenthesize)
            _sql << ')';
        _context.pop_back();
    }


#pragma mark - OPERATION HANDLERS:


    // Handles prefix (unary) operators
    void QueryParser::prefixOp(slice op, Array::iterator& operands) {
        _sql << op;
        if (isalpha(op[op.size-1]))
            _sql << ' ';
        parseNode(operands[0]);
    }


    // Handles postfix operators
    void QueryParser::postfixOp(slice op, Array::iterator& operands) {
        parseNode(operands[0]);
        _sql << " " << op;
    }

    
    // Handles infix operators
    void QueryParser::infixOp(slice op, Array::iterator& operands) {
        int n = 0;
        for (auto &i = operands; i; ++i) {
            if (n++ > 0) {
                if (op != ","_sl)           // special case for argument lists
                    _sql << ' ';
                _sql << op << ' ';
            }
            parseNode(i.value());
        }
    }


    // Handles EXISTS
    void QueryParser::existsOp(slice op, Array::iterator& operands) {
        // "EXISTS propertyname" turns into a call to fl_exists()
        if (writeNestedPropertyOpIfAny("fl_exists", operands))
            return;

        _sql << op;
        if (isalpha(op[op.size-1]))
            _sql << ' ';
        parseNode(operands[0]);
    }

    
    // Handles "x BETWEEN y AND z" expressions
    void QueryParser::betweenOp(slice op, Array::iterator& operands) {
        parseNode(operands[0]);
        _sql << ' ' << op << ' ';
        parseNode(operands[1]);
        _sql << " AND ";
        parseNode(operands[2]);
    }


    // Handles "x IN y" and "x NOT IN y" expressions
    void QueryParser::inOp(slice op, Array::iterator& operands) {
        parseNode(operands.value());
        _sql << ' ' << op << ' ';
        writeArgList(++operands);
    }


    // Handles "property MATCH pattern" expressions (FTS)
    void QueryParser::matchOp(slice op, Array::iterator& operands) {
        // Write the match expression (using an implicit join):
        auto ftsTableNo = FTSPropertyIndex(operands[0]);
        Assert(ftsTableNo > 0);
        _sql << "(FTS" << ftsTableNo << ".text MATCH ";
        parseNode(operands[1]);
        _sql << " AND FTS" << ftsTableNo << ".rowid = " << _tableName << ".sequence)";
    }


    // Handles "ANY var IN array SATISFIES expr" (and EVERY, and ANY AND EVERY)
    void QueryParser::anyEveryOp(slice op, Array::iterator& operands) {
        auto var = (string)operands[0]->asString();
        if (!isValidIdentifier(var))
            fail("ANY/EVERY first parameter must be an identifier; '%s' is not", var.c_str());
        if (_variables.count(var) > 0)
            fail("Variable '%s' is already in use", var.c_str());
        _variables.insert(var);

        string property = propertyFromNode(operands[1]);
        if (property.empty())
            fail("ANY/EVERY only supports a property as its source");

        bool every = (op != "ANY"_sl);
        bool anyAndEvery = (op == "ANY AND EVERY"_sl);

        //OPT: If expr is `var = value`, can generate `fl_contains(array, value)` instead 

        if (anyAndEvery) {
            _sql << '(';
            writePropertyGetter("fl_count", property);
            _sql << " > 0 AND ";
        }

        if (every)
            _sql << "NOT ";
        _sql << "EXISTS (SELECT 1 FROM ";
        writePropertyGetter("fl_each", property);
        _sql << " AS _" << var << " WHERE ";
        if (every)
            _sql << "NOT (";
        parseNode(operands[2]);
        if (every)
            _sql << ')';
        _sql << ')';
        if (anyAndEvery)
            _sql << ')';

        _variables.erase(var);
    }


    // Handles doc property accessors, e.g. [".", "prop"] or [".prop"] --> fl_value(body, "prop")
    void QueryParser::propertyOp(slice op, Array::iterator& operands) {
        writePropertyGetter("fl_value", propertyFromOperands(operands));
    }


    // Handles substituted query parameters, e.g. ["$", "x"] or ["$x"] --> $_x
    void QueryParser::parameterOp(slice op, Array::iterator& operands) {
        alloc_slice parameter;
        if (op.size == 1) {
            parameter = operands[0]->toString();
        } else {
            parameter = op;
            parameter.moveStart(1);
            if (operands.count() > 0)
                fail("extra operands to '%.*s'", splat(parameter));
        }
        auto paramStr = (string)parameter;
        if (!isAlphanumericOrUnderscore(parameter))
            fail("Invalid query parameter name '%.*s'", splat(parameter));
        _parameters.insert(paramStr);
        _sql << "$_" << paramStr;
    }


    // Handles variables used in ANY/EVERY predicates
    void QueryParser::variableOp(slice op, Array::iterator& operands) {
        string var;
        if (op.size == 1) {
            var = (string)operands[0]->asString();
            ++operands;
        } else {
            op.moveStart(1);
            var = op.asString();
        }
        if (!isValidIdentifier(var))
            fail("Invalid variable name '%.*s'", splat(op));
        if (_variables.count(var) == 0)
            fail("No such variable '%.*s'", splat(op));

        if (operands.count() == 0) {
            _sql << '_' << var << ".value";
        } else {
            auto property = propertyFromOperands(operands);
            _sql << "fl_value(_" << var << ".pointer, ";
            writeSQLString(_sql, slice(property));
            _sql << ")";
        }
    }


    // Handles MISSING, which is the N1QL equivalent of NULL
    void QueryParser::missingOp(slice op, Array::iterator& operands) {
        _sql << "NULL";
    }


    // Handles SELECT
    void QueryParser::selectOp(fleece::slice op, Array::iterator &operands) {
        // SELECT is unusual in that its operands are encoded as an object
        auto dict = operands[0]->asDict();
        if (!dict)
            fail("Argument to SELECT must be an object");
        if (_context.size() <= 2) {
            // Outer SELECT
            writeSelect(dict);
        } else {
            // Nested SELECT; use a fresh parser
            QueryParser nested(_tableName, _bodyColumnName);
            nested.parse(dict);
            _sql << nested.SQL();
        }
    }


    // Handles unrecognized operators, based on prefix ('.', '$', '?') or suffix ('()').
    void QueryParser::fallbackOp(slice op, Array::iterator& operands) {
        // Put the actual op into the context instead of a null
        auto operation = *_context.back();
        operation.op = op;
        _context.back() = &operation;

        if (op.size > 0 && op[0] == '.') {
            op.moveStart(1);  // skip '.'
            writePropertyGetter("fl_value", string(op));
        } else if (op.size > 0 && op[0] == '$') {
            parameterOp(op, operands);
        } else if (op.size > 0 && op[0] == '?') {
            variableOp(op, operands);
        } else if (op.size > 2 && op[op.size-2] == '(' && op[op.size-1] == ')') {
            functionOp(op, operands);
        } else {
            fail("Unknown operator '%.*s'", splat(op));
        }
    }


    // Handles function calls, where the op ends with "()"
    void QueryParser::functionOp(slice op, Array::iterator& operands) {
        op.size -= 2;
        string opStr = op.asString();
        for (unsigned i = 0; i < opStr.size(); ++i) {
            if (!isalnum(opStr[i]) && opStr[i] != '_')
                fail("Illegal non-alphanumeric character in function name");
            opStr[i] = (char)tolower(opStr[i]);
        }
        // TODO: Validate that this is a known function

        // Special case: "array_count(propertyname)" turns into a call to fl_count:
        if (opStr == "array_count" && writeNestedPropertyOpIfAny("fl_count", operands))
            return;
        else if (opStr == "rank" && writeNestedPropertyOpIfAny("rank", operands)) {
            return;
        }

        _sql << op;
        writeArgList(operands);
    }


    // Writes operands as a comma-separated list (parenthesized depending on current precedence)
    void QueryParser::writeArgList(Array::iterator& operands) {
        handleOperation(&kArgListOperation, kArgListOperation.op, operands);
    }

    void QueryParser::writeColumnList(Array::iterator& operands) {
        handleOperation(&kColumnListOperation, kColumnListOperation.op, operands);
    }


#pragma mark - PROPERTIES:


    // Concatenates property operands to produce the property path string
    static string propertyFromOperands(Array::iterator &operands) {
        stringstream property;
        int n = 0;
        for (auto &i = operands; i; ++i,++n) {
            auto item = i.value();
            auto arr = item->asArray();
            if (arr) {
                if (n == 0)
                    fail("Property path can't start with an array index");
                // TODO: Support ranges (2 numbers)
                if (arr->count() != 1)
                    fail("Property array index must have exactly one item");
                if (!arr->get(0)->isInteger())
                    fail("Property array index must be an integer");
                auto index = arr->get(0)->asInt();
                property << '[' << index << ']';
            } else {
                slice name = item->asString();
                if (!name)
                    fail("Invalid JSON value in property path");
                if (n > 0)
                    property << '.';
                property << name;
            }
        }
        return property.str();
    }


    // Returns the property represented by a node, or "" if it's not a property node
    static string propertyFromNode(const Value *node) {
        Array::iterator i(node->asArray());
        if (i.count() >= 1) {
            auto op = i[0]->asString();
            if (op && op[0] == '.') {
                if (op.size == 1) {
                    ++i;  // skip "." item
                    return propertyFromOperands(i);
                } else {
                    op.moveStart(1);
                    return (string)op;
                }
            }
        }
        return "";              // not a valid property node
    }


    // If the first operand is a property operation, writes it using the given SQL function name
    // and returns true; else returns false.
    bool QueryParser::writeNestedPropertyOpIfAny(const char *fnName, Array::iterator &operands) {
        if (operands.count() == 0 )
            return false;
        auto property = propertyFromNode(operands[0]);
        if (property.empty())
            return false;
        writePropertyGetter(fnName, property);
        return true;
    }


    // Writes a call to a Fleece SQL function, including the closing ")".
    void QueryParser::writePropertyGetter(const string &fn, const string &property) {
        if (property == "_id") {
            if (fn != "fl_value")
                fail("can't use '_id' in this context");
            _sql << "key";
        } else if (property == "_sequence") {
            if (fn != "fl_value")
                fail("can't use '_sequence' in this context");
            _sql << "sequence";
        } else if (fn == "rank") {
            // FTS rank() needs special treatment
            string fts = FTSIndexName(property);
            if (find(_ftsTables.begin(), _ftsTables.end(), fts) == _ftsTables.end())
                fail("rank() can only be used with FTS properties");
            _sql << "rank(matchinfo(\"" << fts << "\"))";
        } else {
            _sql << fn << "(" << _bodyColumnName << ", ";
            auto path = appendPaths(_propertyPath, property);
            writeSQLString(_sql, slice(path));
            _sql << ")";
        }
    }


    /*static*/ std::string QueryParser::expressionSQL(const fleece::Value* expr,
                                                      const char *bodyColumnName)
    {
        QueryParser qp("XXX", bodyColumnName);
        qp.parseJustExpression(expr);
        return qp.SQL();
    }


#pragma mark - FULL-TEXT-SEARCH MATCH:


    void QueryParser::findFTSProperties(const Value *node) {
        Array::iterator i(node->asArray());
        if (i.count() == 0)
            return;
        slice op = i.value()->asString();
        ++i;
        if (op == "MATCH"_sl && i) {
            FTSPropertyIndex(i.value(), true); // add LHS
            ++i;
        }

        // Recurse into operands:
        for (; i; ++i)
            findFTSProperties(i.value());
    }


    string QueryParser::indexName(const Array *keys) {
        string name = keys->toJSON().asString();
        for (int i = (int)name.size(); i >= 0; --i) {
            if (name[i] == '"')
                name[i] = '\'';
        }
        return _tableName + "::" + name;
    }

    
    string QueryParser::FTSIndexName(const Value *key) {
        slice op = mustBeArray(key)->get(0)->asString();
        if (op.size == 0)
            fail("Invalid left-hand-side of MATCH");
        else if (op[0] == '.')
            return FTSIndexName(propertyFromNode(key));     // abbreviation for common case
        else
            return _tableName + "::" + indexName(key->asArray());
    }

    string QueryParser::FTSIndexName(const string &property) {
        return _tableName + "::." + property;
    }


    size_t QueryParser::FTSPropertyIndex(const Value *matchLHS, bool canAdd) {
        string key = FTSIndexName(matchLHS);
        auto i = find(_ftsTables.begin(), _ftsTables.end(), key);
        if (i != _ftsTables.end()) {
            return i - _ftsTables.begin() + 1;
        } else if (canAdd) {
            _ftsTables.push_back(key);
            return _ftsTables.size();
        } else {
            return 0;
        }
    }

}
