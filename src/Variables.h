// RWX game/units/custom/variables/{VariableSubstitutionParser,ExpressionEvaluator,VariableMap,
// VariableNameValidator}.java as a parse-time pre-pass. CustomUnitConfigParser.java:1220 runs it once
// per unit config, after [core]copyFrom and @copyFromSection have been merged and before a single key
// is read, so every reader downstream only ever sees plain text.
#pragma once
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#if __has_include(<charconv>)
#include <charconv>
#define RP_HAS_CHARCONV 1
#endif
#include "Ini.h"

namespace varsub {

struct Error : std::runtime_error {
    explicit Error(const std::string& m) : std::runtime_error(m) {}
};

// Utility.isNumeric:1314-1322 - digits and '.', and a '-' that is only a '-' at index 0. Nothing else
// passes, and an empty string counts as numeric.
inline bool isNumeric(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || c == '.') continue;
        if (c == '-' && i == 0) continue;
        return false;
    }
    return true;
}

// ExpressionEvaluator.b:150-152 - one operator character anywhere in the expression is what makes it
// numeric, i.e. evaluated instead of pasted in.
inline bool numericMode(const std::string& s) { return s.find_first_of("*/+-()^%") != std::string::npos; }

// Utility.formatNumber:581-586 - an integral double prints without a decimal point.
inline std::string formatNumber(double d) {
    double r = std::trunc(d);
    if (r == d && std::fabs(d) < 2.0e9) return std::to_string((long long)d);
#ifdef RP_HAS_CHARCONV
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, d);
    if (res.ec == std::errc()) return std::string(buf, res.ptr);
#endif
    std::ostringstream os; os << d;
    return os.str();
}

// ExpressionEvaluator.a(String):36-148, the same hand rolled scanner: additive, then multiplicative,
// then a factor that is a sign, a bracket, a run of digits and dots, or a run of lowercase letters that
// takes the *next factor* as its argument (so `sqrt 9` is valid without brackets). Only ASCII spaces are
// skipped, and `^` binds at the factor level, which makes it right associative.
class Expr {
public:
    explicit Expr(const std::string& s) : s(s) {}
    double parse() {
        next();
        double v = additive();
        if (pos < (int)s.size()) throw Error(std::string("Unexpected: ") + s[pos]);
        return v;
    }

private:
    const std::string& s;
    int pos = -1, cur = -1;

    void next() { ++pos; cur = pos < (int)s.size() ? (unsigned char)s[pos] : -1; }
    bool expect(int c) {
        while (cur == ' ') next();
        if (cur == c) { next(); return true; }
        return false;
    }
    double additive() {
        double d = multiplicative();
        for (;;) {
            if (expect('+')) d += multiplicative();
            else if (expect('-')) d -= multiplicative();
            else return d;
        }
    }
    double multiplicative() {
        double d = factor();
        for (;;) {
            if (expect('*')) d *= factor();
            else if (expect('/')) d /= factor();
            else if (expect('%')) d = std::fmod(d, factor());
            else return d;
        }
    }
    double factor() {
        if (expect('+')) return factor();
        if (expect('-')) return -factor();
        int start = pos;
        double v;
        if (expect('(')) {
            v = additive();
            expect(')');
        } else if ((cur >= '0' && cur <= '9') || cur == '.') {
            while ((cur >= '0' && cur <= '9') || cur == '.') next();
            std::string num = s.substr(start, pos - start);
            try { v = std::stod(num); }
            catch (...) { throw Error("Bad number: " + num); }
        } else if (cur >= 'a' && cur <= 'z') {
            while (cur >= 'a' && cur <= 'z') next();
            std::string name = s.substr(start, pos - start);
            double arg = factor();
            const double deg = 3.14159265358979323846 / 180.0;   // Math.toRadians
            if (name == "sqrt") v = std::sqrt(arg);
            else if (name == "sin") v = std::sin(arg * deg);
            else if (name == "cos") v = std::cos(arg * deg);
            else if (name == "tan") v = std::tan(arg * deg);
            else if (name == "int") v = (double)(int)arg;
            else throw Error("Unknown function: " + name);
        } else {
            throw Error(std::string("Unexpected: ") + (cur < 0 ? std::string("<end>") : std::string(1, (char)cur)));
        }
        if (expect('^')) v = std::pow(v, factor());
        return v;
    }
};

// RWX's IniFile is case sensitive for both section and key names (IniFile.java:225-249 stores the text
// as written and :297-318 looks it up with Map.get), and a value of "IGNORE" reads back as missing.
inline const IniSection* findExact(const IniFile& ini, const std::string& name) {
    for (auto& s : ini.sections) if (s.name == name) return &s;
    return nullptr;
}
inline bool getValueExact(const IniSection& sec, const std::string& key, std::string& out) {
    for (auto& kv : sec.kv)
        if (kv.first == key) {
            if (kv.second == "IGNORE") return false;
            out = kv.second;
            return true;
        }
    return false;
}

class Ctx {
public:
    const IniFile& ini;
    // VariableMap is a HashMap keyed by the raw name, so defines and globals are case sensitive too.
    std::map<std::string, std::string> global;

    // ExpressionEvaluator.b:182-204
    std::string lookup(const std::string& name, const std::string& section,
                       const std::map<std::string, std::string>& local) const {
        if (name.find('.') != std::string::npos) {
            std::vector<std::string> parts;
            size_t start = 0;
            for (;;) {
                size_t d = name.find('.', start);
                parts.push_back(name.substr(start, d == std::string::npos ? std::string::npos : d - start));
                if (d == std::string::npos) break;
                start = d + 1;
            }
            if (parts.size() != 2) throw Error("Unexpected key format: " + name);
            std::string sec = parts[0] == "section" ? section : parts[0];
            std::string v;
            const IniSection* is = findExact(ini, sec);
            if (!is || !getValueExact(*is, parts[1], v)) {
                std::string msg = "Static $ block: Could not find: [" + sec + "]" + parts[1] +
                                  " in this conf file";
                // :195-197 - %{} is the dynamic syntax, and $ only reads static blocks
                if (parts[0] == "self") msg += ". Hint: You might have wanted % instead of $ for a dynamic string";
                throw Error(msg);
            }
            if (v.find("${") != std::string::npos)
                throw Error("Reference [" + sec + "]" + parts[1] + " is dynamic, chaining is not yet supported");
            return v;
        }
        auto it = local.find(name);
        if (it != local.end()) return it->second;
        auto gt = global.find(name);
        if (gt != global.end()) return gt->second;
        throw Error("Could not find variable with name: " + name);
    }

    // ExpressionEvaluator.a:154-180
    std::string evaluate(const std::string& raw, const std::string& section,
                         const std::map<std::string, std::string>& local) const {
        std::string e = Trim(raw);
        bool num = numericMode(e);
        std::string out;
        size_t i = 0, guard = 0;
        while (i < e.size()) {
            char c = e[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) { out += c; ++i; continue; }
            size_t j = i + 1;   // Pattern b: [A-Za-z_][A-Za-z_.0-9]*
            while (j < e.size() && ((e[j] >= 'A' && e[j] <= 'Z') || (e[j] >= 'a' && e[j] <= 'z') ||
                                   (e[j] >= '0' && e[j] <= '9') || e[j] == '_' || e[j] == '.'))
                ++j;
            std::string tok = e.substr(i, j - i);
            if (++guard > 100) throw Error("Too many loops while parsing");
            // :166 - only these names are left for the evaluator. `tan` is missing from RWX's list even
            // though its parser supports it, so ${tan 45} fails there as well.
            if (isNumeric(tok) || tok == "int" || tok == "cos" || tok == "sin" || tok == "sqrt") {
                out += tok;
            } else {
                std::string v = lookup(tok, section, local);
                if (num && !isNumeric(v))
                    throw Error("Cannot do maths on '" + v + "' from " + tok + " (not a number)");
                out += v;
            }
            i = j;
        }
        if (!num) return out;
        return formatNumber(Expr(out).parse());
    }
};

// VariableNameValidator.a:11-24
inline void validateName(const std::string& n) {
    if (n.empty()) throw Error("name cannot be empty");
    if (n.find_first_of(" }$.{-+:( ") != std::string::npos) throw Error("invalid character in name");
    if (n[0] >= '0' && n[0] <= '9') throw Error("name cannot start with a digit");
}

// VariableSubstitutionParser.a:27-113. Both registration loops and the substitution walk mirror the
// Java order: @global keys of every section first, then one pass per section whose value text holding
// ${...} is rebuilt, and every rebuilt value written back only once all sections have been read - that
// is why a substitution can never see another substitution's result.
inline void substitute(IniFile& ini, const std::string& file, std::vector<std::string>& warn) {
    Ctx c{ ini, {} };
    auto& global = c.global;
    for (auto& sec : ini.sections) {
        for (auto& kv : sec.kv) {
            std::string lk = ToLower(kv.first);
            if (lk.rfind("@global ", 0) != 0) continue;
            std::string name = Trim(kv.first.substr(8));
            try {
                validateName(name);
                if (findExact(ini, name)) throw Error("A section already has that name");
                if (kv.second.find("${") != std::string::npos)
                    throw Error("has dynamic value: '" + kv.second + "', this is not yet supported");
                global[name] = kv.second;
            } catch (const Error& e) {
                warn.push_back(file + ": [" + sec.name + "]" + kv.first + ": " + e.what());
            }
        }
    }
    struct Pending { std::string sec, key, value; };
    std::vector<Pending> out;
    for (auto& sec : ini.sections) {
        if (sec.name.rfind("comment_", 0) == 0 || sec.name.rfind("template_", 0) == 0) continue;
        std::map<std::string, std::string> local;
        for (auto& kv : sec.kv) {
            std::string lk = ToLower(kv.first);
            if (lk.rfind("@define ", 0) != 0) continue;
            std::string name = Trim(kv.first.substr(8));
            try {
                validateName(name);
                if (findExact(ini, name)) throw Error("A section already has that name");
                if (kv.second.find("${") != std::string::npos)
                    throw Error("has dynamic value: '" + kv.second + "', this is not yet supported");
                local[name] = kv.second;
            } catch (const Error& e) {
                warn.push_back(file + ": [" + sec.name + "]" + kv.first + ": " + e.what());
            }
        }
        for (auto& kv : sec.kv) {
            if (kv.second.find("${") == std::string::npos) continue;
            std::string rebuilt;
            size_t i = 0;
            try {
                int guard = 0;
                while (i < kv.second.size()) {
                    size_t open = kv.second.find("${", i);
                    if (open == std::string::npos) { rebuilt += kv.second.substr(i); break; }
                    size_t close = kv.second.find('}', open + 2);
                    if (close == std::string::npos) { rebuilt += kv.second.substr(i); break; }
                    if (++guard > 100) throw Error("Too many loops while parsing");
                    rebuilt += kv.second.substr(i, open - i);
                    rebuilt += c.evaluate(kv.second.substr(open + 2, close - open - 2), sec.name, local);
                    i = close + 1;
                }
                out.push_back({ sec.name, kv.first, rebuilt });
            } catch (const Error& e) {
                warn.push_back(file + ": [" + sec.name + "]" + kv.first + ": " + e.what());
            }
        }
    }
    for (auto& p : out) {
        IniSection* s = ini.find(p.sec);
        if (s) s->set(p.key, p.value);
    }
}

} // namespace varsub
