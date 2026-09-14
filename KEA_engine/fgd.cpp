// ============================================================================
// fgd.cpp - FGD (Forge Game Data) parser implementation
// ============================================================================
//
// The parser is deliberately tolerant: the .fgd is an editor hint file, so a
// typo in it must never crash the engine. On any malformed class/attribute it
// stops that class, records an error, and keeps whatever was parsed cleanly.
//
// Grammar handled (standard TrenchBroom flavour):
//   file      := ( class )*
//   class     := '@' (PointClass|SolidClass|BaseClass) options '=' name
//                [':' quoted] '[' keyvalues* ']'
//   options   := ( base(...) | size(...) | model(...) | color(...) |
//                  spawnflags(...) | iconsprite(...) )*
//   keyvalue  := key '(' type ')' [':' default] [':' help]
//                ['=' '[' (choices|flags) ']' ]
//   choices   := value ':' quoted  |  value ':' quoted ':' default
// Comments are '//' and '/* ... */'. Commas are treated as separators.
// ============================================================================
#include "fgd.h"
#include <fstream>
#include <iterator>
#include <cstdlib>
using namespace std;

namespace {

enum class TokKind { Ident, String, Punct, End };

struct Token {
    TokKind kind = TokKind::End;
    string text;
    int line = 0;
};

struct Scanner {
    const string& src;
    size_t pos = 0;
    int line = 1;

    explicit Scanner(const string& s) : src(s) {}

    char peek() const { return pos < src.size() ? src[pos] : '\0'; }
    char peekAt(size_t off) const { return pos + off < src.size() ? src[pos + off] : '\0'; }
    char get() { char c = src[pos++]; if (c == '\n') line++; return c; }

    // Skips whitespace, commas and // / /* */ comments.
    void skipIgnored() {
        while (pos < src.size()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') { get(); continue; }
            if (c == '/' && peekAt(1) == '/') { while (pos < src.size() && peek() != '\n') get(); continue; }
            if (c == '/' && peekAt(1) == '*') {
                get(); get();
                while (pos < src.size() && !(peek() == '*' && peekAt(1) == '/')) get();
                if (pos < src.size()) { get(); get(); }
                continue;
            }
            break;
        }
    }

    Token next() {
        skipIgnored();
        int tokLine = line;
        if (pos >= src.size()) return Token{ TokKind::End, "", tokLine };

        char c = peek();
        if (c == '"') {
            get();
            string s;
            while (pos < src.size()) {
                char ch = get();
                if (ch == '"') break;
                if (ch == '\\' && pos < src.size()) { s.push_back(get()); continue; }
                s.push_back(ch);
            }
            return Token{ TokKind::String, s, tokLine };
        }
        if (c == '=' || c == ':' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
            get();
            return Token{ TokKind::Punct, string(1, c), tokLine };
        }
        if (c == '@') {
            // Fold the '@' onto the following identifier: @PointClass etc.
            get();
            string s("@");
            while (pos < src.size()) {
                char nc = peek();
                if (nc == ' ' || nc == '\t' || nc == '\r' || nc == '\n' || nc == ','
                    || nc == '=' || nc == '(' || nc == ')' || nc == '[' || nc == ']' || nc == '"') break;
                s.push_back(get());
            }
            return Token{ TokKind::Ident, s, tokLine };
        }
        string s;
        while (pos < src.size()) {
            char nc = peek();
            if (nc == ' ' || nc == '\t' || nc == '\r' || nc == '\n' || nc == ','
                || nc == '=' || nc == ':' || nc == '(' || nc == ')' || nc == '[' || nc == ']'
                || nc == '{' || nc == '}' || nc == '"' || nc == '/') break;
            s.push_back(get());
        }
        if (s.empty()) { get(); return Token{ TokKind::Punct, string(1, c), tokLine }; }
        return Token{ TokKind::Ident, s, tokLine };
    }
};

bool isIdent(const Token& t, const char* s) { return t.kind == TokKind::Ident && t.text == s; }
bool isPunct(const Token& t, const char* s) { return t.kind == TokKind::Punct && t.text == s; }

bool parseFloat(const string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    float v = (float)strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

// Consumes `( name , name ... )`. Names may be quoted or bare.
void parseBaseList(const vector<Token>& toks, size_t& i, vector<string>& out) {
    i++; // '('
    while (i < toks.size() && !isPunct(toks[i], ")")) {
        const Token& t = toks[i];
        if (t.kind == TokKind::String || t.kind == TokKind::Ident) out.push_back(t.text);
        i++;
    }
    if (i < toks.size()) i++; // ')'
}

// Reads three number tokens into a vec3, leaving the index past them.
bool parseVec3(const vector<Token>& toks, size_t& i, glm::vec3& out) {
    int count = 0;
    while (i < toks.size() && count < 3) {
        float v;
        if (toks[i].kind != TokKind::Ident || !parseFloat(toks[i].text, v)) break;
        out[count] = v;
        count++;
        i++;
    }
    return count == 3;
}

// Consumes an option such as base(...), size(...), model(...) or color(...).
// Returns false when the token at i is not a class option we handle.
bool parseClassOption(const vector<Token>& toks, size_t& i, FgdClass& cls) {
    const Token& head = toks[i];

    if (isIdent(head, "base") && isPunct(toks[i + 1], "(")) { i++; parseBaseList(toks, i, cls.baseClasses); return true; }

    if (isIdent(head, "size") && isPunct(toks[i + 1], "(")) {
        i++; // 'size'
        i++; // '('
        if (!parseVec3(toks, i, cls.sizeMin)) return true;
        parseVec3(toks, i, cls.sizeMax);
        if (i < toks.size() && isPunct(toks[i], ")")) i++;
        return true;
    }

    if (isIdent(head, "model") && isPunct(toks[i + 1], "(")) {
        i++; // 'model'
        i++; // '('
        if (i < toks.size() && isPunct(toks[i], "{")) {
            // Dynamic placeholder: model({ "path": key, "scale": key })
            i++; // '{'
            while (i < toks.size() && !isPunct(toks[i], "}")) {
                string name;
                if (toks[i].kind == TokKind::String || toks[i].kind == TokKind::Ident) { name = toks[i].text; i++; }
                if (i < toks.size() && isPunct(toks[i], ":")) i++;
                string value;
                if (i < toks.size() && (toks[i].kind == TokKind::String || toks[i].kind == TokKind::Ident)) { value = toks[i].text; i++; }
                if (name == "path") cls.modelPathKey = value;
                else if (name == "scale") cls.modelScaleKey = value;
            }
            if (i < toks.size()) i++; // '}'
        }
        else if (i < toks.size() && (toks[i].kind == TokKind::String || toks[i].kind == TokKind::Ident)) {
            // Hardcoded: model("path/to/mesh.obj")
            cls.modelSpec = toks[i].text;
            i++;
        }
        if (i < toks.size() && isPunct(toks[i], ")")) i++;
        return true;
    }

    if (isIdent(head, "color") || isIdent(head, "spawnflags")
        || isIdent(head, "iconsprite") || isIdent(head, "modelspawnflag")) {
        // Editor-only options we do not need - skip ( ... ) contents.
        i++;
        int depth = 0;
        while (i < toks.size()) {
            if (isPunct(toks[i], "(")) depth++;
            else if (isPunct(toks[i], ")")) { depth--; if (depth == 0) { i++; break; } }
            i++;
        }
        return true;
    }

    return false;
}

// Parses a class starting at toks[i] (which must be an @*Class token) and
// leaves i past its closing ']'. Returns false with err set on malformed data.
bool parseClass(const vector<Token>& toks, size_t& i, FgdClass& cls, string& err) {
    if (isIdent(toks[i], "@PointClass")) cls.type = FgdClassType::Point;
    else if (isIdent(toks[i], "@SolidClass")) cls.type = FgdClassType::Solid;
    else if (isIdent(toks[i], "@BaseClass")) cls.type = FgdClassType::Base;
    else { err = "expected @PointClass/@SolidClass/@BaseClass at line " + to_string(toks[i].line); return false; }
    i++;

    while (i < toks.size()) {
        if (toks[i].kind == TokKind::Ident && parseClassOption(toks, i, cls)) continue;
        break;
    }

    if (!(i < toks.size() && isPunct(toks[i], "="))) {
        err = "expected '=' after class options for class at line " + to_string(toks[i].line)
            + " (got '" + toks[i].text + "' kind " + to_string((int)toks[i].kind) + ")";
        return false;
    }
    i++;

    if (i < toks.size() && (toks[i].kind == TokKind::Ident || toks[i].kind == TokKind::String)) {
        cls.name = toks[i].text;
        i++;
    }
    if (i < toks.size() && isPunct(toks[i], ":")) {
        i++;
        if (i < toks.size() && toks[i].kind == TokKind::String) { cls.description = toks[i].text; i++; }
    }

    if (!(i < toks.size() && isPunct(toks[i], "["))) {
        err = "expected '[' to open keyvalues for class '" + cls.name + "' at line " + to_string(toks[i].line);
        return false;
    }
    i++;

    while (i < toks.size() && !isPunct(toks[i], "]")) {
        FgdKeyValue kv;

        if (toks[i].kind == TokKind::String || toks[i].kind == TokKind::Ident) {
            kv.key = toks[i].text;
            i++;
        }
        else {
            err = "expected key name for class '" + cls.name + "' at line " + to_string(toks[i].line)
                + " (got '" + toks[i].text + "' kind " + to_string((int)toks[i].kind) + ")";
            return false;
        }

        if (i < toks.size() && isPunct(toks[i], "(")) {
            i++;
            if (i < toks.size() && (toks[i].kind == TokKind::Ident || toks[i].kind == TokKind::String)) {
                kv.type = toks[i].text;
                i++;
            }
            if (i < toks.size() && isPunct(toks[i], ")")) i++;
        }

        // Optional fields after the type: key(type) : "Display" : default : "Help".
        // Any of the last three may be omitted; an empty slot is a bare ':'.
        if (i < toks.size() && isPunct(toks[i], ":")) {
            i++;
            if (i < toks.size() && (toks[i].kind == TokKind::String || toks[i].kind == TokKind::Ident)) {
                kv.displayName = toks[i].text;
                i++;
            }
            if (i < toks.size() && isPunct(toks[i], ":")) {
                i++;
                if (i < toks.size() && (toks[i].kind == TokKind::String || toks[i].kind == TokKind::Ident)) {
                    kv.defaultValue = toks[i].text;
                    i++;
                }
                if (i < toks.size() && isPunct(toks[i], ":")) {
                    i++;
                    if (i < toks.size() && toks[i].kind == TokKind::String) {
                        kv.description = toks[i].text;
                        i++;
                    }
                }
            }
        }

        // Optional '=' then a '[...]' choices or flags block.
        if (i < toks.size() && isPunct(toks[i], "=")) i++;
        if (i < toks.size() && isPunct(toks[i], "[")) {
            i++;
            while (i < toks.size() && !isPunct(toks[i], "]")) {
                int value = 0;
                bool hasValue = false;
                float vf;
                if (toks[i].kind == TokKind::Ident && parseFloat(toks[i].text, vf)) {
                    value = (int)vf;
                    hasValue = true;
                }
                i++;
                string name;
                if (i < toks.size() && isPunct(toks[i], ":")) {
                    i++;
                    if (i < toks.size() && toks[i].kind == TokKind::String) { name = toks[i].text; i++; }
                }
                bool defVal = false;
                if (i < toks.size() && isPunct(toks[i], ":")) {
                    i++;
                    if (i < toks.size() && (toks[i].kind == TokKind::Ident || toks[i].kind == TokKind::String)) {
                        defVal = (toks[i].text == "1");
                        i++;
                    }
                }
                if (!hasValue) continue;
                if (kv.type == "flags") {
                    kv.isFlags = true;
                    kv.flags.push_back(FgdFlag{ value, name, defVal });
                }
                else {
                    kv.isChoices = true;
                    kv.choices.push_back(make_pair(value, name));
                }
            }
            if (i < toks.size()) i++; // ']'
        }

        if (kv.isFlags) kv.type = "flags";
        else if (kv.isChoices) kv.type = "choices";
        cls.keyValues.push_back(kv);
    }

    if (i < toks.size()) i++; // ']'
    return true;
}

} // namespace

FgdFile FgdParser::load(const string& path) {
    FgdFile result;

    ifstream file(path, ios::binary);
    if (!file.is_open()) {
        result.error = "FgdParser: could not open file: " + path;
        return result;
    }
    string src((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());

    Scanner scanner(src);
    vector<Token> toks;
    for (;;) {
        Token t = scanner.next();
        toks.push_back(t);
        if (t.kind == TokKind::End) break;
    }

    size_t i = 0;
    while (i < toks.size()) {
        const Token& t = toks[i];
        if (t.kind == TokKind::Ident
            && (t.text == "@PointClass" || t.text == "@SolidClass" || t.text == "@BaseClass")) {
            FgdClass cls;
            string err;
            if (!parseClass(toks, i, cls, err)) {
                result.error = "FgdParser: " + err;
                break;
            }
            if (!cls.name.empty()) result.classes.push_back(cls);
            continue;
        }
        i++; // skip unrecognised tokens
    }

    result.loaded = result.error.empty();
    return result;
}

const FgdClass* FgdFile::find(const string& name) const {
    for (const FgdClass& c : classes)
        if (c.name == name) return &c;
    return nullptr;
}
