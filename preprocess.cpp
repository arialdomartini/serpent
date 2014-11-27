#include <stdio.h>
#include <iostream>
#include <vector>
#include <map>
#include "util.h"
#include "lllparser.h"
#include "bignum.h"
#include "rewriteutils.h"
#include "optimize.h"
#include "preprocess.h"

// Convert a function of the form (def (f x y z) (do stuff)) into
// (if (first byte of ABI is correct) (seq (setup x y z) (do stuff)))
Node convFunction(Node node, int functionCount) {
    std::string prefix = "_temp"+mkUniqueToken()+"_";
    Metadata m = node.metadata;
    std::vector<std::string> varNames;
    std::vector<std::string> longVarNames;
    std::vector<bool> longVarIsArray;
    if (node.args.size() != 2)
        err("Malformed def!", m);
    // Collect the list of variable names and variable byte counts
    for (unsigned i = 0; i < node.args[0].args.size(); i++) {
        if (node.args[0].args[i].val == ":") {
            if (node.args[0].args[i].args.size() != 2)
                err("Malformed def!", m);
            longVarNames.push_back(node.args[0].args[i].args[0].val);
            std::string tag = node.args[0].args[i].args[1].val;
            if (tag == "s")
                longVarIsArray.push_back(false);
            else if (tag == "a")
                longVarIsArray.push_back(true);
            else
                err("Function value can only be string or array", m);
        }
        else {
            varNames.push_back(node.args[0].args[i].val);
        }
    }
    std::vector<Node> sub;
    sub.push_back(asn("comment", token("FUNCTION "+node.args[0].val, m), m));
    if (!varNames.size() && !longVarNames.size()) {
        // do nothing if we have no arguments
    }
    else {
        // First, we declare all the variables that we are going to use
        // for lengths and the static variables
        std::vector<Node> varNameTokens;
        for (unsigned i = 0; i < longVarNames.size(); i++) {
            varNameTokens.push_back(token("_len_"+longVarNames[i], m));
        }
        for (unsigned i = 0; i < varNames.size(); i++) {
            varNameTokens.push_back(token(varNames[i], m));
        }
        sub.push_back(astnode("declare", varNameTokens, m));
        if (varNameTokens.size()) {
            sub.push_back(asn("calldatacopy",
                              asn("ref", varNameTokens[0], m),
                              tkn("1", m),
                              tkn(utd(varNameTokens.size() * 32), m),
                              m));
        }
        // Copy over long variables
        if (longVarNames.size() > 0) {
            std::string pattern = "(with $tot 0 (seq ";
            for (unsigned i = 0; i < longVarNames.size(); i++) {
                std::string var = longVarNames[i];
                std::string varlen = "_len_"+longVarNames[i];
                pattern +=
                    "     (set "+var+" (alloc "+varlen+"))           "
                  + "     (calldatacopy "+var+" $tot "+varlen+")   "
                  + "     (set $tot (add $tot "+varlen+"))         ";
            }
            pattern += "))";
            sub.push_back(subst(parseLLL(pattern), msn(), prefix, m));
        }
    }
    // And the actual code
    sub.push_back(node.args[1]);
    // Main LLL-based function body
    return astnode("if",
                   astnode("eq",
                           astnode("get", token("__funid", m), m),
                           token(unsignedToDecimal(functionCount), m),
                           m),
                   astnode("seq", sub, m));
}

// Populate an svObj with the arguments needed to determine
// the storage position of a node
svObj getStorageVars(svObj pre, Node node, std::string prefix,
                     int index) {
    Metadata m = node.metadata;
    if (!pre.globalOffset.size()) pre.globalOffset = "0";
    std::vector<Node> h;
    std::vector<std::string> coefficients;
    // Array accesses or atoms
    if (node.val == "access" || node.type == TOKEN) {
        std::string tot = "1";
        h = listfyStorageAccess(node);
        coefficients.push_back("1");
        for (unsigned i = h.size() - 1; i >= 1; i--) {
            // Array sizes must be constant or at least arithmetically
            // evaluable at compile time
            if (!isPureArithmetic(h[i]))
                err("Array size must be fixed value", m);
            // Create a list of the coefficient associated with each
            // array index
            coefficients.push_back(decimalMul(coefficients.back(), h[i].val));
        }
    }
    // Tuples
    else {
        int startc;
        // Handle the (fun <fun_astnode> args...) case
        if (node.val == "fun") {
            startc = 1;
            h = listfyStorageAccess(node.args[0]);
        }
        // Handle the (<fun_name> args...) case, which
        // the serpent parser produces when the function
        // is a simple name and not a complex astnode
        else {
            startc = 0;
            h = listfyStorageAccess(token(node.val, m));
        }
        svObj sub = pre;
        sub.globalOffset = "0";
        // Evaluate tuple elements recursively
        for (unsigned i = startc; i < node.args.size(); i++) {
            sub = getStorageVars(sub,
                                 node.args[i],
                                 prefix+h[0].val.substr(2)+".",
                                 i-startc);
        }
        coefficients.push_back(sub.globalOffset);
        for (unsigned i = h.size() - 1; i >= 1; i--) {
            // Array sizes must be constant or at least arithmetically
            // evaluable at compile time
            if (!isPureArithmetic(h[i]))
               err("Array size must be fixed value", m);
            // Create a list of the coefficient associated with each
            // array index
            coefficients.push_back(decimalMul(coefficients.back(), h[i].val));
        }
        pre.offsets = sub.offsets;
        pre.coefficients = sub.coefficients;
        pre.nonfinal = sub.nonfinal;
        pre.nonfinal[prefix+h[0].val.substr(2)] = true;
    }
    pre.coefficients[prefix+h[0].val.substr(2)] = coefficients;
    pre.offsets[prefix+h[0].val.substr(2)] = pre.globalOffset;
    pre.indices[prefix+h[0].val.substr(2)] = index;
    if (decimalGt(tt176, coefficients.back()))
        pre.globalOffset = decimalAdd(pre.globalOffset, coefficients.back());
    return pre;
}

// Preprocess input containing functions
//
// localExterns is a map of the form, eg,
//
// { x: { foo: 0, bar: 1, baz: 2 }, y: { qux: 0, foo: 1 } ... }
//
// localExternSigs is a map of the form, eg,
//
// { x : { foo: iii, bar: iis, baz: ia }, y: { qux: i, foo: as } ... }
//
// Signifying that x.foo = 0, x.baz = 2, y.foo = 1, etc
// and that x.foo has three integers as arguments, x.bar has two
// integers and a variable-length string, and baz has an integer
// and an array
//
// globalExterns is a one-level map, eg from above
//
// { foo: 1, bar: 1, baz: 2, qux: 0 }
//
// globalExternSigs is a one-level map, eg from above
//
// { foo: as, bar: iis, baz: ia, qux: i}
//
// Note that globalExterns and globalExternSigs may be ambiguous
// Also, a null signature implies an infinite tail of integers
preprocessResult preprocess(Node inp) {
    Node x = inp.args[0];
    inp = x;
    Metadata m = inp.metadata;
    if (inp.val != "seq")
        inp = astnode("seq", inp, m);
    std::vector<Node> empty = std::vector<Node>();
    Node init = astnode("seq", empty, m);
    Node shared = astnode("seq", empty, m);
    std::vector<Node> any;
    std::vector<Node> functions;
    preprocessAux out = preprocessAux();
    out.localExterns["self"] = std::map<std::string, int>();
    int functionCount = 0;
    int storageDataCount = 0;
    for (unsigned i = 0; i < inp.args.size(); i++) {
        Node obj = inp.args[i];
        // Functions
        if (obj.val == "def") {
            if (obj.args.size() == 0)
                err("Empty def", m);
            std::string funName = obj.args[0].val;
            // Init, shared and any are special functions
            if (funName == "init" || funName == "shared" || funName == "any") {
                if (obj.args[0].args.size())
                    err(funName+" cannot have arguments", m);
            }
            if (funName == "init") init = obj.args[1];
            else if (funName == "shared") shared = obj.args[1];
            else if (funName == "any") any.push_back(obj.args[1]);
            else {
                // Other functions
                functions.push_back(convFunction(obj, functionCount));
                out.localExterns["self"][obj.args[0].val] = functionCount;
                functionCount++;
            }
        }
        // Extern declarations
        else if (obj.val == "extern") {
            std::string externName = obj.args[0].args[0].val;
            Node al = obj.args[0].args[1];
            if (!out.localExterns.count(externName))
                out.localExterns[externName] = std::map<std::string, int>();
            for (unsigned i = 0; i < al.args.size(); i++) {
                if (al.args[i].val == ":") {
                    std::string v = al.args[i].args[0].val;
                    std::string sig = al.args[i].args[1].val;
                    out.globalExterns[v] = i;
                    out.globalExternSigs[v] = sig;
                    out.localExterns[externName][v] = i;
                    out.localExternSigs[externName][v] = sig;
                }
                else {
                    std::string v = al.args[i].val;
                    out.globalExterns[v] = i;
                    out.globalExternSigs[v] = "";
                    out.localExterns[externName][v] = i;
                    out.localExternSigs[externName][v] = "";
                }
            }
        }
        // Storage variables/structures
        else if (obj.val == "data") {
            out.storageVars = getStorageVars(out.storageVars,
                                             obj.args[0],
                                             "",
                                             storageDataCount);
            storageDataCount += 1;
        }
        else any.push_back(obj);
    }
    std::vector<Node> main;
    if (shared.args.size()) main.push_back(shared);
    if (init.args.size()) main.push_back(init);

    std::vector<Node> code;
    if (shared.args.size()) code.push_back(shared);
    for (unsigned i = 0; i < any.size(); i++)
        code.push_back(any[i]);
    for (unsigned i = 0; i < functions.size(); i++)
        code.push_back(functions[i]);
    Node codeNode;
    if (functions.size() > 0) {
        codeNode = astnode("with",
                           token("__funid", m),
                           astnode("byte",
                                   token("0", m),
                                   astnode("calldataload", token("0", m), m),
                                   m),
                           astnode("seq", code, m),
                           m);
    }
    else codeNode = astnode("seq", code, m);
    main.push_back(astnode("~return",
                           token("0", m),
                           astnode("lll",
                                   codeNode,
                                   token("0", m),
                                   m),
                           m));



    return preprocessResult(astnode("seq", main, inp.metadata), out);
}