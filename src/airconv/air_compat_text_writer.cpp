#include "air_compat_text_writer.hpp"

#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/Support/Error.h>

namespace dxmt::aircompat {

namespace {

bool isIdentChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$' ||
         c == '-';
}

std::string readValueName(const std::string &s, size_t pos, size_t &end) {
  end = pos;
  if (pos >= s.size() || (s[pos] != '%' && s[pos] != '@'))
    return {};
  size_t i = pos + 1;
  if (i < s.size() && s[i] == '"') {
    i++;
    while (i < s.size() && s[i] != '"')
      i++;
    if (i < s.size())
      i++;
  } else {
    while (i < s.size() && isIdentChar(s[i]))
      i++;
  }
  end = i;
  return s.substr(pos, i - pos);
}

// `load` and `store` may carry `atomic` and `volatile` ahead of the type.
// Returns them as written, each followed by a space, and moves pos past them.
std::string readMemoryQualifiers(const std::string &s, size_t &pos) {
  std::string quals;
  for (;;) {
    while (pos < s.size() && std::isspace((unsigned char)s[pos]))
      pos++;
    if (s.compare(pos, 7, "atomic ") == 0) {
      quals += "atomic ";
      pos += 7;
    } else if (s.compare(pos, 9, "volatile ") == 0) {
      quals += "volatile ";
      pos += 9;
    } else {
      return quals;
    }
  }
}

// Read one LLVM type starting at pos (skipping leading whitespace), including
// "addrspace(N)"/"*" pointer suffix. Stops right after the type.
std::string readFullType(const std::string &s, size_t pos, size_t &end) {
  while (pos < s.size() && std::isspace((unsigned char)s[pos]))
    pos++;
  size_t i = pos;
  if (i >= s.size()) {
    end = i;
    return {};
  }
  char c = s[i];
  if (c == '<' || c == '[' || c == '{') {
    int depth = 0;
    for (; i < s.size(); i++) {
      char d = s[i];
      if (d == '<' || d == '[' || d == '{')
        depth++;
      else if (d == '>' || d == ']' || d == '}') {
        depth--;
        if (depth == 0) {
          i++;
          break;
        }
      } else if (d == '"') {
        i++;
        while (i < s.size() && s[i] != '"')
          i++;
      }
    }
  } else if (c == '%' || c == '@') {
    size_t e;
    readValueName(s, i, e);
    i = e;
  } else {
    while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_'))
      i++;
  }
  for (;;) {
    size_t j = i;
    while (j < s.size() && std::isspace((unsigned char)s[j]))
      j++;
    if (s.compare(j, std::strlen("addrspace("), "addrspace(") == 0) {
      size_t close = s.find(')', j);
      if (close == std::string::npos)
        break;
      i = close + 1;
      continue;
    }
    if (j < s.size() && s[j] == '*') {
      i = j + 1;
      continue;
    }
    break;
  }
  size_t e = i;
  while (e > pos && std::isspace((unsigned char)s[e - 1]))
    e--;
  end = i;
  return s.substr(pos, e - pos);
}

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a]))
    a++;
  while (b > a && std::isspace((unsigned char)s[b - 1]))
    b--;
  return s.substr(a, b - a);
}

std::string leadingWhitespace(const std::string &s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a]))
    a++;
  return s.substr(0, a);
}

bool startsWith(const std::string &s, const char *p) {
  return s.compare(0, std::strlen(p), p) == 0;
}

std::vector<std::string> splitTopLevel(const std::string &s) {
  std::vector<std::string> out;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '<' || c == '[' || c == '{' || c == '(')
      depth++;
    else if (c == '>' || c == ']' || c == '}' || c == ')')
      depth--;
    else if (c == '"') {
      i++;
      while (i < s.size() && s[i] != '"')
        i++;
    } else if (c == ',' && depth == 0) {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  out.push_back(s.substr(start));
  return out;
}

// Apple AIR opaque-struct (or element) pointee for pointer arg `argIdx` of an
// air intrinsic named `fn` (no leading '@'), or "" if not a known handle.
std::string airIntrinsicArgPointee(const std::string &fn, int argIdx) {
  size_t tpos = fn.find("texture_");
  if (tpos == std::string::npos)
    tpos = fn.find("depth_");
  if (tpos != std::string::npos) {
    size_t e = tpos;
    while (e < fn.size() && fn[e] != '.')
      e++;
    std::string kind = fn.substr(tpos, e - tpos);
    bool hasSampler = fn.find("sample") != std::string::npos;
    if (argIdx == 0)
      return "%struct._" + kind + "_t";
    if (argIdx == 1 && hasSampler)
      return "%struct._sampler_t";
    return "";
  }
  if (fn.find("atomic") != std::string::npos && argIdx == 0) {
    size_t dot = fn.rfind('.');
    std::string suf = dot == std::string::npos ? "" : fn.substr(dot + 1);
    if (suf == "f32")
      return "float";
    if (suf == "i64")
      return "i64";
    if (suf == "f16")
      return "half";
    if (suf == "i16")
      return "i16";
    return "i32";
  }
  return "";
}

std::string typedPointer(const std::string &pointee, const std::string &as) {
  std::string out = pointee;
  if (!as.empty())
    out += " " + as;
  out += "*";
  return out;
}

bool isOpaqueHandle(const std::string &pointee) {
  if (pointee.find(' ') != std::string::npos || pointee.find('*') != std::string::npos)
    return false;
  return pointee.rfind("%struct._", 0) == 0 &&
         (pointee.find("_texture_") != std::string::npos ||
          pointee.find("_sampler_") != std::string::npos ||
          pointee.find("_depth_") != std::string::npos);
}

// Match "ptr" or "ptr addrspace(N)" at p (word boundary); set as / after.
bool matchPtrType(const std::string &s, size_t p, std::string &as,
                  size_t &after) {
  if (s.compare(p, 3, "ptr") != 0)
    return false;
  if (p > 0 && isIdentChar(s[p - 1]))
    return false;
  size_t q = p + 3;
  if (q < s.size() && isIdentChar(s[q]))
    return false;
  as.clear();
  size_t r = q;
  while (r < s.size() && std::isspace((unsigned char)s[r]))
    r++;
  if (s.compare(r, std::strlen("addrspace("), "addrspace(") == 0) {
    size_t close = s.find(')', r);
    if (close != std::string::npos) {
      as = s.substr(r, close - r + 1);
      q = close + 1;
    }
  }
  after = q;
  return true;
}

std::string addrspaceOf(const std::string &ptrType) {
  std::string as;
  size_t after;
  size_t p = ptrType.find("ptr");
  if (p != std::string::npos && matchPtrType(ptrType, p, as, after))
    return as;
  return "";
}

/* The address space of a typed pointer ("i32 addrspace(1)*" ->
 * "addrspace(1)"), in the form matchPtrType gives; "" for addrspace 0. */
std::string typedPointerAddrspace(const std::string &typed) {
  size_t a = typed.rfind("addrspace(");
  if (a == std::string::npos)
    return "";
  size_t c = typed.find(')', a);
  return c == std::string::npos ? "" : typed.substr(a, c - a + 1);
}

std::string airResourceStructKind(std::string kind) {
  for (size_t i = 1; i < kind.size(); i++) {
    if (std::isdigit((unsigned char)kind[i]) && kind[i - 1] != '_') {
      kind.insert(i, "_");
      i++;
    }
  }
  return kind;
}

std::string airArgTypeToLLVMFieldType(const std::string &name) {
  if (name == "ulong")
    return "i64";
  if (name == "uint4")
    return "<4 x i32> addrspace(2)*";
  if (name == "int4")
    return "<4 x i32> addrspace(2)*";
  if (name == "float4")
    return "<4 x float> addrspace(2)*";
  if (name == "sampler")
    return "%struct._sampler_t addrspace(2)*";
  if (name.rfind("texture", 0) == 0) {
    size_t kindEnd = name.find('<');
    std::string kind = kindEnd == std::string::npos ? name : name.substr(0, kindEnd);
    kind = airResourceStructKind(kind);
    return "%struct._" + kind + "_t addrspace(1)*";
  }
  if (name.rfind("depth", 0) == 0) {
    size_t kindEnd = name.find('<');
    std::string kind = kindEnd == std::string::npos ? name : name.substr(0, kindEnd);
    kind = airResourceStructKind(kind);
    return "%struct._" + kind + "_t addrspace(1)*";
  }
  return "";
}

/* The LLVM type of a Metal scalar or vector type name, "" when unknown. */
std::string airScalarLLVMType(const std::string &name) {
  static const std::map<std::string, std::string> scalars = {
      {"bool", "i1"},   {"char", "i8"},   {"uchar", "i8"},  {"short", "i16"},
      {"ushort", "i16"}, {"int", "i32"},  {"uint", "i32"},  {"long", "i64"},
      {"ulong", "i64"}, {"half", "half"}, {"float", "float"},
  };
  if (auto it = scalars.find(name); it != scalars.end())
    return it->second;
  if (name.size() > 1 && name.back() >= '2' && name.back() <= '4') {
    auto it = scalars.find(name.substr(0, name.size() - 1));
    if (it != scalars.end() && it->second != "i1")
      return "<" + name.substr(name.size() - 1) + " x " + it->second + ">";
  }
  return "";
}

/* The type of one argument-buffer field, from the node describing the
 * argument it holds: a buffer is a pointer into its address space, an
 * indirect constant the value itself, anything else (textures, samplers)
 * goes by its type name. "" when it cannot be told. */
std::string airArgumentFieldType(const std::string &argNode, const std::string &typeName) {
  auto quoted = [&](const char *key) -> std::string {
    size_t k = argNode.find(key);
    if (k == std::string::npos)
      return "";
    size_t q1 = argNode.find("!\"", k + std::strlen(key));
    if (q1 == std::string::npos)
      return "";
    q1 += 2;
    size_t q2 = argNode.find('"', q1);
    return q2 == std::string::npos ? "" : argNode.substr(q1, q2 - q1);
  };
  std::string argType = quoted("!\"air.arg_type_name\"");
  if (argType.empty())
    argType = typeName;
  if (argNode.find("!\"air.buffer\"") != std::string::npos) {
    int space = 1;
    size_t as = argNode.find("!\"air.address_space\", i32 ");
    if (as != std::string::npos)
      space = std::atoi(argNode.c_str() + as + std::strlen("!\"air.address_space\", i32 "));
    std::string pointee = airScalarLLVMType(argType);
    if (pointee.empty())
      pointee = "i8";
    return pointee + " addrspace(" + std::to_string(space) + ")*";
  }
  if (argNode.find("!\"air.indirect_constant\"") != std::string::npos)
    return airScalarLLVMType(argType);
  return airArgTypeToLLVMFieldType(argType);
}

std::string pointerPointeeType(const std::string &ptrType) {
  std::string s = trim(ptrType);
  if (s.empty() || s.back() != '*')
    return "";
  s.pop_back();
  while (!s.empty() && std::isspace((unsigned char)s.back()))
    s.pop_back();
  size_t asp = s.rfind(" addrspace(");
  if (asp != std::string::npos) {
    size_t close = s.find(')', asp);
    if (close != std::string::npos && close == s.size() - 1) {
      s.erase(asp);
      while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    }
  }
  return s;
}

std::string downgradeOpaquePtrTokens(const std::string &s, bool *changed = nullptr) {
  std::string out;
  bool any = false;
  size_t i = 0;
  while (i < s.size()) {
    std::string as;
    size_t after;
    if (matchPtrType(s, i, as, after)) {
      out += typedPointer("i8", as);
      i = after;
      any = true;
      continue;
    }
    out += s[i];
    i++;
  }
  if (changed)
    *changed = any;
  return out;
}

// ---------------------------------------------------------------------------
// Floating point literals. LLVM 23 prints them in forms the IR parser in
// air-as rejects: the shortest decimal that round-trips in the type (0.1f as
// "1.000000e-01", which an old parser refuses as inexact), "f0x<bits>" for
// floats, and "+inf", "-qnan", "+nan(0x..)", "+snan(0x..)". Rewrite those the
// way LLVM 22 printed them: a float as the hex of the equal double, a double as
// its bits, half as 0xH and bfloat as 0xR. Exact decimals are left alone.

const llvm::fltSemantics *fpSemantics(const std::string &type) {
  if (type == "half")
    return &llvm::APFloat::IEEEhalf();
  if (type == "bfloat")
    return &llvm::APFloat::BFloat();
  if (type == "float")
    return &llvm::APFloat::IEEEsingle();
  if (type == "double")
    return &llvm::APFloat::IEEEdouble();
  return nullptr;
}

std::string legacyFPLiteral(const llvm::APFloat &value, const std::string &type) {
  char buf[32];
  if (type == "half" || type == "bfloat") {
    snprintf(buf, sizeof(buf), type == "half" ? "0xH%04llX" : "0xR%04llX",
             (unsigned long long)value.bitcastToAPInt().getZExtValue());
    return buf;
  }
  llvm::APFloat apf = value;
  if (type == "float") {
    // As LLVM 22's AsmWriter: a signaling NaN is quieted by the conversion,
    // so rebuild it from the converted bits.
    bool snan = apf.isSignaling(), ignored;
    apf.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &ignored);
    if (snan) {
      llvm::APInt payload = apf.bitcastToAPInt();
      apf = llvm::APFloat::getSNaN(llvm::APFloat::IEEEdouble(), apf.isNegative(), &payload);
    }
  }
  snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)apf.bitcastToAPInt().getZExtValue());
  return buf;
}

// Reads one floating point literal of the given type at s[pos]. Sets keep when
// the text is a decimal an old parser takes as is.
bool readFPLiteral(const std::string &s, size_t pos, const std::string &type,
                   llvm::APFloat &value, size_t &end, bool &keep) {
  const llvm::fltSemantics &sem = *fpSemantics(type);
  unsigned bits = llvm::APFloat::getSizeInBits(sem);
  size_t i = pos;
  bool neg = false;
  keep = false;

  auto hexRun = [&](size_t from, size_t &to) {
    to = from;
    while (to < s.size() && std::isxdigit((unsigned char)s[to]))
      to++;
    return to > from;
  };

  if (s.compare(i, 3, "f0x") == 0) {
    size_t e;
    if (!hexRun(i + 3, e))
      return false;
    value = llvm::APFloat(sem, llvm::APInt(bits, s.substr(i + 3, e - i - 3), 16));
    end = e;
    return true;
  }
  if (s[i] == '+' || s[i] == '-') {
    neg = s[i] == '-';
    i++;
    if (s.compare(i, 3, "inf") == 0) {
      value = llvm::APFloat::getInf(sem, neg);
      end = i + 3;
      return true;
    }
    if (s.compare(i, 4, "qnan") == 0) {
      value = llvm::APFloat::getQNaN(sem, neg);
      end = i + 4;
      return true;
    }
    bool snan = s.compare(i, 5, "snan(") == 0;
    if (snan || s.compare(i, 4, "nan(") == 0) {
      size_t h = i + (snan ? 5 : 4), e;
      if (s.compare(h, 2, "0x") != 0 || !hexRun(h + 2, e) || e >= s.size() || s[e] != ')')
        return false;
      llvm::APInt payload(bits, s.substr(h + 2, e - h - 2), 16);
      value = snan ? llvm::APFloat::getSNaN(sem, neg, &payload) : llvm::APFloat::getQNaN(sem, neg, &payload);
      end = e + 1;
      return true;
    }
  }

  // [-+]digits.digits e[-+]digits, as LLVM prints decimals
  size_t d = i;
  while (d < s.size() && std::isdigit((unsigned char)s[d]))
    d++;
  if (d == i || d >= s.size() || s[d] != '.')
    return false;
  size_t f = ++d;
  while (d < s.size() && std::isdigit((unsigned char)s[d]))
    d++;
  if (d == f || d + 1 >= s.size() || (s[d] != 'e' && s[d] != 'E') || (s[d + 1] != '+' && s[d + 1] != '-'))
    return false;
  size_t x = d += 2;
  while (d < s.size() && std::isdigit((unsigned char)s[d]))
    d++;
  if (d == x)
    return false;
  std::string text = s.substr(pos, d - pos);
  auto parse = [&](llvm::APFloat &v) {
    auto status = v.convertFromString(text, llvm::APFloat::rmNearestTiesToEven);
    if (status)
      return true;
    llvm::consumeError(status.takeError());
    return false;
  };
  value = llvm::APFloat(sem);
  if (!parse(value))
    return false;
  // An old parser reads the decimal as a double and refuses it unless the
  // type holds that double exactly; doubles are always fine.
  llvm::APFloat asDouble(llvm::APFloat::IEEEdouble());
  bool lost = false;
  if (parse(asDouble))
    asDouble.convert(sem, llvm::APFloat::rmNearestTiesToEven, &lost);
  keep = type == "double" || !lost;
  end = d;
  return true;
}

// The scalar type of the operands of an fadd/fsub/fmul/fdiv/frem/fneg/fcmp/phi
// on this line (the element type for vectors), whose constant operands are not
// preceded by their type.
std::string fpOperandType(const std::string &line) {
  static const char *const ops[] = {"fadd ", "fsub ", "fmul ", "fdiv ", "frem ", "fneg ", "fcmp ", "phi "};
  static const char *const flags[] = {"nnan", "ninf", "nsz", "arcp", "contract", "afn", "reassoc", "fast"};
  size_t eq = line.find(" = ");
  size_t p = eq == std::string::npos ? line.find_first_not_of(" \t") : eq + 3;
  if (p == std::string::npos)
    return {};
  const char *op = nullptr;
  for (const char *o : ops)
    if (line.compare(p, std::strlen(o), o) == 0)
      op = o;
  if (!op)
    return {};
  p += std::strlen(op);
  for (bool more = true; more;) {
    more = false;
    for (const char *fl : flags) {
      size_t n = std::strlen(fl);
      if (line.compare(p, n, fl) == 0 && p + n < line.size() && line[p + n] == ' ') {
        p += n + 1;
        more = true;
      }
    }
  }
  if (!std::strcmp(op, "fcmp ")) {  // the predicate
    p = line.find(' ', p);
    if (p == std::string::npos)
      return {};
    p++;
  }
  if (line.compare(p, 1, "<") == 0) {  // <N x type>
    size_t x = line.find(" x ", p);
    if (x == std::string::npos)
      return {};
    p = x + 3;
  }
  for (const char *t : {"half", "bfloat", "float", "double"})
    if (line.compare(p, std::strlen(t), t) == 0)
      return t;
  return {};
}

std::string downgradeFPLiterals(const std::string &line) {
  std::string out;
  std::string lineType;
  bool lineTypeKnown = false, quoted = false;

  for (size_t i = 0; i < line.size();) {
    char c = line[i];
    if (c == '"')
      quoted = !quoted;
    bool boundary = i == 0 || std::strchr(" <[(,", line[i - 1]);
    if (quoted || !boundary || !(std::isdigit((unsigned char)c) || c == '+' || c == '-' || c == 'f')) {
      out += c;
      i++;
      continue;
    }

    // The type is the keyword right before the literal, else the operand
    // type of the instruction.
    std::string type;
    size_t k = out.find_last_not_of(' ');
    if (k != std::string::npos) {
      size_t b = out.find_last_of(" <[(,", k);
      std::string prev = out.substr(b == std::string::npos ? 0 : b + 1, k - (b == std::string::npos ? 0 : b + 1) + 1);
      if (fpSemantics(prev))
        type = prev;
    }
    if (type.empty()) {
      if (!lineTypeKnown) {
        lineType = fpOperandType(line);
        lineTypeKnown = true;
      }
      type = lineType;
    }

    llvm::APFloat value(0.0);
    size_t end;
    bool keep;
    if (!type.empty() && readFPLiteral(line, i, type, value, end, keep) &&
        (end == line.size() || std::strchr(" ,>])", line[end]))) {
      if (keep)
        out.append(line, i, end - i);
      else
        out += legacyFPLiteral(value, type);
      i = end;
      continue;
    }
    out += c;
    i++;
  }
  return out;
}

} // namespace

// ===========================================================================
//
// DXMT emits opaque-pointer LLVM22 IR where a single pointer value (an argument
// buffer) is byte-addressed and reinterpreted at many types. Apple's air-as is
// a strict typed-pointer assembler. The reliable downgrade therefore models
// every local pointer as a byte pointer (i8 addrspace(N)*) and inserts an
// explicit `bitcast` at each typed access (load / store / typed GEP / texture
// & sampler intrinsic argument). Globals keep their declared type.
//
// ===========================================================================

bool DowngradeToAppleAIR(const std::string &in, std::string &out,
                         std::string &err) {
  std::vector<std::string> lines;
  {
    std::stringstream ss(in);
    std::string line;
    while (std::getline(ss, line))
      lines.push_back(line);
  }

  // Strip GEP flags that Apple's air-as does not understand.
  auto stripGepFlags = [](std::string s) {
    struct R {
      const char *f, *t;
    };
    static const R reps[] = {
        {"getelementptr inbounds nuw nusw ", "getelementptr inbounds "},
        {"getelementptr inbounds nuw ", "getelementptr inbounds "},
        {"getelementptr inbounds nusw ", "getelementptr inbounds "},
        {"getelementptr nuw ", "getelementptr "},
        {"getelementptr nusw ", "getelementptr "},
        // poison-generating / no-op flags from newer LLVM IR
        {"or disjoint ", "or "},
        {"zext nneg ", "zext "},
        {"trunc nuw nsw ", "trunc "},
        {"trunc nuw ", "trunc "},
        {"trunc nsw ", "trunc "},
        {"icmp samesign ", "icmp "},
    };
    for (const auto &r : reps) {
      size_t p = 0, fl = std::strlen(r.f);
      while ((p = s.find(r.f, p)) != std::string::npos) {
        s.replace(p, fl, r.t);
        p += std::strlen(r.t);
      }
    }
    return s;
  };
  for (auto &l : lines)
    l = stripGepFlags(l);

  // Declared value type of every module-level global ("@name" -> type), read
  // from its "@name = ... global|constant <type> ..." declaration line. Used
  // so references to a global at an unrelated pointee type (e.g. a threadgroup
  // counter passed to a linked-in helper function that expects a byte
  // pointer) get an explicit bitcast instead of a bare mismatched operand,
  // which Apple's air-as rejects outright.
  std::map<std::string, std::string> globalScalarType;
  for (const std::string &line : lines) {
    std::string t = trim(line);
    if (t.empty() || t[0] != '@')
      continue;
    size_t eq = t.find(" = ");
    if (eq == std::string::npos)
      continue;
    std::string name = t.substr(0, eq);
    size_t gp = t.find(" global ", eq);
    size_t cp = t.find(" constant ", eq);
    size_t kw = std::string::npos, kwLen = 0;
    if (gp != std::string::npos && (cp == std::string::npos || gp < cp)) {
      kw = gp;
      kwLen = std::strlen(" global ");
    } else if (cp != std::string::npos) {
      kw = cp;
      kwLen = std::strlen(" constant ");
    }
    if (kw == std::string::npos)
      continue;
    size_t e;
    std::string ty = readFullType(t, kw + kwLen, e);
    if (!ty.empty())
      globalScalarType[name] = ty;
  }

  std::map<std::string, std::vector<std::string>> argBufferFieldOverrides;
  std::map<int, std::string> argBufferInputStructs;
  std::map<std::string, std::string> metadataLines;
  for (const std::string &line : lines) {
    std::string t = trim(line);
    if (t.size() > 1 && t[0] == '!') {
      size_t eq = t.find(" = ");
      if (eq != std::string::npos)
        metadataLines[t.substr(0, eq)] = t;
    }
  }

  // Function(s) actually exported as a shader stage entry point (air.vertex /
  // air.fragment / air.kernel / air.object / air.mesh). argBufferInputStructs
  // below records "parameter index -> struct type" purely by index, meaning
  // it must only ever be applied when rewriting one of THESE functions'
  // signature - applying it to any other `define` in the module (e.g. a
  // tessellation/MSAD helper linked in from a separate .metal file, whose
  // unrelated parameter happens to share the same index) corrupts that
  // helper's signature and produces a type mismatch air-as rejects.
  std::set<std::string> entryPointFunctions;
  for (const char *kind :
       {"air.vertex", "air.fragment", "air.kernel", "air.object", "air.mesh"}) {
    auto it = metadataLines.find(std::string("!") + kind);
    if (it == metadataLines.end())
      continue;
    const std::string &line = it->second;
    size_t p = 0;
    while ((p = line.find('!', p + 1)) != std::string::npos) {
      size_t e = p + 1;
      while (e < line.size() && std::isdigit((unsigned char)line[e]))
        e++;
      if (e == p + 1)
        continue;
      auto ref = metadataLines.find(line.substr(p, e - p));
      if (ref == metadataLines.end())
        continue;
      size_t fp = ref->second.find("ptr @");
      if (fp == std::string::npos)
        continue;
      size_t ne;
      std::string fn = readValueName(ref->second, fp + 4, ne);
      if (!fn.empty())
        entryPointFunctions.insert(fn.substr(1));
    }
  }

  for (const auto &[id, line] : metadataLines) {
    if (line.find("!\"air.indirect_buffer\"") == std::string::npos ||
        line.find("!\"air.struct_type_info\"") == std::string::npos ||
        line.find("!\"air.arg_type_name\"") == std::string::npos)
      continue;

    size_t stp = line.find("!\"air.struct_type_info\"");
    size_t refp = line.find('!', stp + 1);
    if (refp == std::string::npos)
      continue;
    size_t refe = refp + 1;
    while (refe < line.size() && std::isdigit((unsigned char)line[refe]))
      refe++;
    std::string structInfoRef = line.substr(refp, refe - refp);

    size_t atn = line.find("!\"air.arg_type_name\"", stp);
    size_t q1 = line.find("!\"", atn + 1);
    if (q1 == std::string::npos)
      continue;
    q1 += 2;
    size_t q2 = line.find('"', q1);
    if (q2 == std::string::npos)
      continue;
    std::string structName = "%" + line.substr(q1, q2 - q1);
    int inputIndex = -1;
    {
      size_t ib = line.find("!{i32 ");
      if (ib != std::string::npos)
        inputIndex = std::atoi(line.c_str() + ib + std::strlen("!{i32 "));
    }

    auto it = metadataLines.find(structInfoRef);
    if (it == metadataLines.end())
      continue;
    std::string info = it->second;
    /* One field per "air.indirect_argument" entry. A field whose type cannot
     * be told leaves the struct as LLVM wrote it: dropping it would move every
     * field after it. */
    std::vector<std::string> fields;
    bool complete = true;
    size_t p = 0;
    while ((p = info.find("!\"", p)) != std::string::npos) {
      p += 2;
      size_t e = info.find('"', p);
      if (e == std::string::npos)
        break;
      std::string typeName = info.substr(p, e - p);
      size_t next = info.find("!\"air.indirect_argument\"", e);
      if (next == std::string::npos)
        break;
      std::string argNode;
      size_t ref = info.find('!', next + 1);
      if (ref != std::string::npos) {
        size_t refEnd = ref + 1;
        while (refEnd < info.size() && std::isdigit((unsigned char)info[refEnd]))
          refEnd++;
        auto node = metadataLines.find(info.substr(ref, refEnd - ref));
        if (node != metadataLines.end())
          argNode = node->second;
      }
      std::string fieldType = airArgumentFieldType(argNode, typeName);
      if (fieldType.empty())
        complete = false;
      fields.push_back(fieldType);
      p = next + 1;
    }
    if (!fields.empty() && complete)
      argBufferFieldOverrides[structName] = fields;
    if (inputIndex >= 0)
      argBufferInputStructs[inputIndex] = structName;
  }

  for (std::string &line : lines) {
    std::string t = trim(line);
    if (t.size() > 1 && t[0] == '%' && t.find(" = type {") != std::string::npos) {
      std::string name = t.substr(0, t.find(' '));
      auto it = argBufferFieldOverrides.find(name);
      if (it != argBufferFieldOverrides.end()) {
        std::string rebuilt = name + " = type { ";
        for (size_t i = 0; i < it->second.size(); i++) {
          if (i)
            rebuilt += ", ";
          rebuilt += it->second[i];
        }
        rebuilt += " }";
        line = leadingWhitespace(line) + rebuilt;
      } else {
        size_t ob = t.find('{');
        size_t cb = t.rfind('}');
        if (ob != std::string::npos && cb != std::string::npos && cb > ob) {
          auto fields = splitTopLevel(t.substr(ob + 1, cb - ob - 1));
          bool changed = false;
          std::string rebuilt = name + " = type { ";
          for (size_t i = 0; i < fields.size(); i++) {
            bool fieldChanged = false;
            std::string field = downgradeOpaquePtrTokens(trim(fields[i]), &fieldChanged);
            changed |= fieldChanged;
            if (i)
              rebuilt += ", ";
            rebuilt += field;
          }
          rebuilt += " }";
          if (changed)
            line = leadingWhitespace(line) + rebuilt;
        }
      }
    }
  }

  std::set<std::string> definedStructs;
  std::map<std::string, std::vector<std::string>> structFields;
  for (const std::string &line : lines) {
    std::string t = trim(line);
    if (t.size() > 1 && t[0] == '%' && t.find(" = type") != std::string::npos) {
      std::string name = t.substr(0, t.find(' '));
      definedStructs.insert(name);
      size_t ob = t.find('{');
      size_t cb = t.rfind('}');
      if (ob != std::string::npos && cb != std::string::npos && cb > ob) {
        auto fs = splitTopLevel(t.substr(ob + 1, cb - ob - 1));
        std::vector<std::string> fields;
        for (auto &f : fs)
          fields.push_back(trim(f));
        structFields[name] = fields;
      }
    }
  }

  // Compute the pointee type of a GEP result by walking aggregate indices.
  // The first index steps over the source type; subsequent indices descend.
  auto gepResultPointee = [&](const std::string &srcT,
                              const std::vector<std::string> &idxVals)
      -> std::string {
    std::string cur = srcT;
    for (size_t k = 1; k < idxVals.size(); k++) {
      if (cur.size() && cur[0] == '[') {
        size_t x = cur.find(" x ");
        if (x == std::string::npos)
          break;
        cur = trim(cur.substr(x + 3, cur.size() - (x + 3) - 1));
      } else if (cur.size() && cur[0] == '<') {
        size_t x = cur.find(" x ");
        if (x == std::string::npos)
          break;
        cur = trim(cur.substr(x + 3, cur.size() - (x + 3) - 1));
      } else if (cur.size() && cur[0] == '%') {
        auto it = structFields.find(cur);
        if (it == structFields.end())
          break;
        // index value is the last whitespace-separated token
        const std::string &iv = idxVals[k];
        size_t sp = iv.rfind(' ');
        std::string num = sp == std::string::npos ? iv : iv.substr(sp + 1);
        bool digits = !num.empty();
        for (char ch : num)
          if (!std::isdigit((unsigned char)ch))
            digits = false;
        if (!digits)
          break;
        size_t fi = (size_t)std::strtoul(num.c_str(), nullptr, 10);
        if (fi >= it->second.size())
          break;
        cur = it->second[fi];
      } else {
        break;
      }
    }
    return cur;
  };

  std::set<std::string> neededOpaqueStructs;
  std::map<std::string, std::string> funcSig; // fn -> "<ret> (<types>)*"
  // Current typed-pointer of each local SSA pointer value, as air-as will see
  // it (reset per function). Lets us bitcast from the correct source type.
  std::map<std::string, std::string> cano;
  int freshCounter = 0;
  auto fresh = [&]() {
    return std::string("%aircompat") + std::to_string(freshCounter++);
  };

  // ---- line-local transforms -------------------------------------------
  auto convertAttributes = [](std::string s) -> std::string {
    struct Rep {
      const char *from;
      const char *to;
    };
    static const Rep reps[] = {
        {"memory(argmem: readwrite)", "argmemonly"},
        {"memory(argmem: read)", "argmemonly readonly"},
        {"memory(argmem: write)", "argmemonly writeonly"},
        {"memory(read)", "readonly"},
        {"memory(write)", "writeonly"},
        {"memory(none)", "readnone"},
        {"captures(none)", "nocapture"},
    };
    for (const auto &r : reps) {
      size_t pos = 0, flen = std::strlen(r.from);
      while ((pos = s.find(r.from, pos)) != std::string::npos) {
        s.replace(pos, flen, r.to);
        pos += std::strlen(r.to);
      }
    }
    // Drop newer attribute keywords that air-as rejects.
    static const char *unknownAttrs[] = {
        "nocreateundeforpoison", "nocallback", "nosanitize_bounds",
    };
    for (const char *attr : unknownAttrs) {
      std::string pat = std::string(" ") + attr;
      size_t pos = 0;
      while ((pos = s.find(pat, pos)) != std::string::npos)
        s.erase(pos, pat.size());
    }
    size_t cp;
    while ((cp = s.find("captures(")) != std::string::npos) {
      size_t close = s.find(')', cp);
      if (close == std::string::npos)
        break;
      size_t e = close + 1;
      if (e < s.size() && s[e] == ' ')
        e++;
      s.erase(cp, e - cp);
    }
    // `range(i32 lo, hi)` on a return value or parameter: newer than air-as,
    // and only an optimisation hint.
    size_t rp = 0;
    while ((rp = s.find("range(", rp)) != std::string::npos) {
      if (rp && s[rp - 1] != ' ' && s[rp - 1] != '(') {
        rp += 6;
        continue;
      }
      size_t close = s.find(')', rp);
      if (close == std::string::npos)
        break;
      size_t e = close + 1;
      if (e < s.size() && s[e] == ' ')
        e++;
      s.erase(rp, e - rp);
    }
    return s;
  };

  auto expandSplat = [](std::string s) -> std::string {
    size_t pos = 0;
    while ((pos = s.find("splat (", pos)) != std::string::npos) {
      size_t open = pos + 6;
      int depth = 0;
      size_t i = open;
      for (; i < s.size(); i++) {
        if (s[i] == '(')
          depth++;
        else if (s[i] == ')') {
          depth--;
          if (depth == 0)
            break;
        }
      }
      if (i >= s.size())
        break;
      std::string inner = s.substr(open + 1, i - open - 1);
      int width = 0;
      size_t lt = s.rfind('<', pos);
      if (lt != std::string::npos) {
        size_t k = lt + 1;
        while (k < s.size() && std::isspace((unsigned char)s[k]))
          k++;
        size_t ks = k;
        while (k < s.size() && std::isdigit((unsigned char)s[k]))
          k++;
        if (k > ks)
          width = std::stoi(s.substr(ks, k - ks));
      }
      if (width <= 0)
        width = 1;
      std::string expanded = "<";
      for (int w = 0; w < width; w++) {
        if (w)
          expanded += ", ";
        expanded += inner;
      }
      expanded += ">";
      s.replace(pos, i - pos + 1, expanded);
      pos += expanded.size();
    }
    return s;
  };

  auto funcNameOf = [&](const std::string &line, size_t from) -> std::string {
    size_t at = line.find('@', from);
    if (at == std::string::npos)
      return {};
    size_t ne;
    std::string fn = readValueName(line, at, ne);
    return fn.empty() ? "" : fn.substr(1);
  };

  // Convert every "ptr[ addrspace(N)]" type token in a line to "i8[ addrspace
  // (N)]*". Used for instructions that do not impose a specific pointee type
  // (icmp, phi, select, ptrtoint, br, ret, ...).
  auto genericI8 = [&](std::string s) -> std::string {
    return downgradeOpaquePtrTokens(s);
  };

  // Emit a pointer operand of the required pointee type. Locals are byte
  // pointers and get a bitcast; globals already carry their declared type.
  auto emitPtrOperand = [&](const std::string &pointee, const std::string &as,
                            const std::string &nm, const std::string &indent,
                            std::vector<std::string> &result)
      -> std::pair<std::string, std::string> {
    std::string typ = typedPointer(pointee, as);
    if (isOpaqueHandle(pointee))
      neededOpaqueStructs.insert(pointee);
    if (nm.empty())
      return {typ, nm};
    std::string from;
    if (nm[0] == '@') {
      // A global's operand type must match its own declaration; if the
      // expected pointee differs, bitcast rather than emitting a mismatched
      // type (air-as validates operand types against the global's real type).
      auto git = globalScalarType.find(nm);
      if (git == globalScalarType.end())
        return {typ, nm}; // not a variable we tracked (e.g. a function ref)
      from = typedPointer(git->second, as);
    } else {
      auto it = cano.find(nm);
      from = it != cano.end() ? it->second : typedPointer("i8", as);
    }
    if (from == typ)
      return {typ, nm}; // already the right type
    std::string tmp = fresh();
    result.push_back(indent + tmp + " = bitcast " + from + " " + nm + " to " +
                     typ);
    return {typ, tmp};
  };

  // Rewrite a define/declare signature: pointer params become i8 addrspace(N)*
  // (or the handle type for air-intrinsic declarations). Records funcSig.
  auto rewriteSig = [&](const std::string &line) -> std::string {
    bool isDecl = startsWith(trim(line), "declare");
    size_t kw = isDecl ? line.find("declare") + 7 : line.find("define") + 6;
    size_t at = line.find('@', kw);
    if (at == std::string::npos)
      return line;
    std::string fn = funcNameOf(line, kw);
    bool airFn = fn.rfind("air.", 0) == 0;
    std::string retSeg = trim(line.substr(kw, at - kw));
    size_t e;
    std::string ret = readFullType(retSeg, 0, e);
    if (ret.empty())
      ret = retSeg;
    size_t op = line.find('(', at);
    if (op == std::string::npos)
      return line;
    int depth = 0;
    size_t cl = std::string::npos;
    for (size_t i = op; i < line.size(); i++) {
      if (line[i] == '(')
        depth++;
      else if (line[i] == ')') {
        depth--;
        if (depth == 0) {
          cl = i;
          break;
        }
      }
    }
    if (cl == std::string::npos)
      return line;
    std::string body = line.substr(op + 1, cl - op - 1);
    std::string rebuilt = line.substr(0, op + 1);
    std::vector<std::string> sigTypes;
    if (!trim(body).empty()) {
      auto args = splitTopLevel(body);
      for (size_t i = 0; i < args.size(); i++) {
        std::string a = args[i];
        size_t pp = a.find("ptr");
        std::string as;
        size_t after;
        if (pp != std::string::npos && matchPtrType(a, pp, as, after)) {
          std::string pointee = airFn ? airIntrinsicArgPointee(fn, (int)i) : "";
          if (!airFn && entryPointFunctions.count(fn)) {
            auto ab = argBufferInputStructs.find((int)i);
            if (ab != argBufferInputStructs.end())
              pointee = ab->second;
          }
          if (pointee.empty())
            pointee = "i8";
          if (isOpaqueHandle(pointee))
            neededOpaqueStructs.insert(pointee);
          a = a.substr(0, pp) + typedPointer(pointee, as) + a.substr(after);
          // record the parameter's typed pointer for body bitcasts
          size_t lp = a.rfind('%');
          if (lp != std::string::npos) {
            size_t ne2;
            std::string an = readValueName(a, lp, ne2);
            if (!an.empty())
              cano[an] = typedPointer(pointee, as);
          }
        }
        size_t te;
        sigTypes.push_back(readFullType(a, 0, te));
        if (i)
          rebuilt += ",";
        rebuilt += a;
      }
    }
    rebuilt += line.substr(cl);
    std::string sig = ret + " (";
    for (size_t i = 0; i < sigTypes.size(); i++) {
      if (i)
        sig += ", ";
      sig += sigTypes[i];
    }
    sig += ")*";
    if (!fn.empty())
      funcSig[fn] = sig;
    return rebuilt;
  };

  // Rewrite one function-body instruction into `result` (may prepend bitcasts).
  auto rewriteBody = [&](const std::string &line,
                         std::vector<std::string> &result) {
    std::string indent = leadingWhitespace(line);
    std::string t = trim(line);

    // load: %R = load ACC, ptr<as> %p <rest>
    size_t lpos = line.find(" = load ");
    if (lpos != std::string::npos && line.find("call") == std::string::npos) {
      std::string lhs = trim(line.substr(0, lpos));
      size_t p = lpos + std::strlen(" = load ");
      std::string quals = readMemoryQualifiers(line, p);
      size_t e;
      std::string acc = readFullType(line, p, e);
      if (argBufferFieldOverrides.count(acc)) {
        size_t comma = line.find(',', e);
        if (comma != std::string::npos) {
          size_t pe;
          std::string ptrTy = readFullType(line, comma + 1, pe);
          size_t q = pe;
          while (q < line.size() && std::isspace((unsigned char)line[q]))
            q++;
          size_t ne;
          std::string nm = readValueName(line, q, ne);
          std::string rest = line.substr(ne);
          std::string as;
          size_t after;
          size_t ptrName = ptrTy.rfind(acc);
          if (ptrName != std::string::npos) {
            size_t suffix = ptrName + acc.size();
            std::string maybePtr = "ptr " + ptrTy.substr(suffix);
            matchPtrType(maybePtr, 0, as, after);
          }
          auto sf = structFields.find(acc);
          auto of = argBufferFieldOverrides.find(acc);
          const std::vector<std::string> *fields =
              sf != structFields.end() ? &sf->second
                                       : (of != argBufferFieldOverrides.end() ? &of->second : nullptr);
          if (fields && !fields->empty()) {
            std::string gep = fresh();
            result.push_back(indent + gep + " = getelementptr inbounds " +
                             acc + ", " + typedPointer(acc, as) + " " + nm +
                             ", i32 0, i32 0");
            std::string fieldPtr = typedPointer((*fields)[0], as);
            result.push_back(indent + lhs + " = load " + quals + (*fields)[0] +
                             ", " + fieldPtr + " " + gep + rest);
            cano[lhs] = (*fields)[0];
            return;
          }
        }
      }
      size_t pp = line.find("ptr", e);
      std::string as;
      size_t after;
      if (pp != std::string::npos && matchPtrType(line, pp, as, after)) {
        size_t q = after;
        while (q < line.size() && std::isspace((unsigned char)line[q]))
          q++;
        size_t ne;
        std::string nm = readValueName(line, q, ne);
        std::string rest = line.substr(ne);
        std::string accType = acc;
        if (startsWith(acc, "ptr") || argBufferFieldOverrides.count(acc)) {
          std::string argStruct;
          for (const auto &[idx, st] : argBufferInputStructs) {
            (void)idx;
            auto ct = cano.find(nm);
            if (ct != cano.end() && ct->second == typedPointer(st, as)) {
              argStruct = st;
              break;
            }
          }
          if (!argStruct.empty()) {
            auto sf = structFields.find(argStruct);
            auto of = argBufferFieldOverrides.find(argStruct);
            const std::vector<std::string> *fields =
                sf != structFields.end() ? &sf->second
                                         : (of != argBufferFieldOverrides.end() ? &of->second : nullptr);
            if (fields && !fields->empty()) {
              std::string gep = fresh();
              result.push_back(indent + gep + " = getelementptr inbounds " +
                               argStruct + ", " + typedPointer(argStruct, as) +
                               " " + nm + ", i32 0, i32 0");
              std::string fieldPtr = typedPointer((*fields)[0], as);
              result.push_back(indent + lhs + " = load " + quals + (*fields)[0] +
                               ", " + fieldPtr + " " + gep + rest);
              cano[lhs] = (*fields)[0];
              return;
            }
          }
          auto knownPtr = cano.find(nm);
          std::string knownPointee =
              knownPtr == cano.end() ? "" : pointerPointeeType(knownPtr->second);
          accType = knownPointee.empty()
                        ? typedPointer("i8", addrspaceOf(acc))
                        : knownPointee;
        }
        auto pr = emitPtrOperand(accType, as, nm, indent, result);
        // if the loaded value is itself a pointer, track its type
        if (startsWith(acc, "ptr"))
          cano[lhs] = accType;
        result.push_back(indent + lhs + " = load " + quals + accType + ", " + pr.first +
                         " " + pr.second + rest);
        return;
      }
    }

    // store: store VT val, ptr<as> %p <rest>
    if (startsWith(t, "store ")) {
      size_t sp = line.find("store ");
      size_t p = sp + std::strlen("store ");
      std::string quals = readMemoryQualifiers(line, p);
      size_t e;
      std::string vt = readFullType(line, p, e);
      size_t pp = line.find("ptr", e);
      std::string as;
      size_t after;
      if (pp != std::string::npos && matchPtrType(line, pp, as, after)) {
        size_t comma = line.rfind(',', pp);
        std::string valText = line.substr(e, comma - e); // " %6"
        size_t q = after;
        while (q < line.size() && std::isspace((unsigned char)line[q]))
          q++;
        size_t ne;
        std::string nm = readValueName(line, q, ne);
        std::string rest = line.substr(ne);
        std::string vType =
            startsWith(vt, "ptr") ? typedPointer("i8", addrspaceOf(vt)) : vt;
        auto pr = emitPtrOperand(vType, as, nm, indent, result);
        result.push_back(indent + "store " + quals + vType + valText + ", " + pr.first +
                         " " + pr.second + rest);
        return;
      }
    }

    // cmpxchg / atomicrmw: the pointer operand points at the value type.
    //   %R = cmpxchg [weak] [volatile] ptr<as> %p, T %cmp, T %new <rest>
    //   %R = atomicrmw [volatile] OP ptr<as> %p, T %v <rest>
    {
      size_t kw = line.find(" = cmpxchg ");
      size_t kwlen = std::strlen(" = cmpxchg ");
      if (kw == std::string::npos) {
        kw = line.find(" = atomicrmw ");
        kwlen = std::strlen(" = atomicrmw ");
      }
      size_t pp = kw == std::string::npos ? std::string::npos : line.find("ptr", kw + kwlen);
      std::string as;
      size_t after;
      if (pp != std::string::npos && matchPtrType(line, pp, as, after)) {
        size_t q = after;
        while (q < line.size() && std::isspace((unsigned char)line[q]))
          q++;
        size_t ne;
        std::string nm = readValueName(line, q, ne);
        size_t comma = line.find(',', ne);
        size_t te;
        std::string vt = comma == std::string::npos ? "" : readFullType(line, comma + 1, te);
        if (!nm.empty() && !vt.empty() && !startsWith(vt, "ptr")) {
          auto pr = emitPtrOperand(vt, as, nm, indent, result);
          result.push_back(line.substr(0, pp) + pr.first + " " + pr.second + line.substr(ne));
          return;
        }
      }
    }

    // getelementptr: %R = getelementptr [inbounds] SrcT, ptr<as> %base <idx>
    size_t gp = line.find("getelementptr");
    if (gp != std::string::npos) {
      size_t eqp = line.find(" = ");
      std::string lhs = eqp != std::string::npos ? trim(line.substr(0, eqp)) : "";
      size_t tp = gp + std::strlen("getelementptr");
      std::string after_kw = line.substr(tp);
      size_t skip = 0;
      while (skip < after_kw.size() && std::isspace((unsigned char)after_kw[skip]))
        skip++;
      bool inbounds = after_kw.compare(skip, 8, "inbounds") == 0;
      if (inbounds)
        skip += 8;
      size_t e;
      std::string srcT = readFullType(line, tp + skip, e);
      size_t pp = line.find("ptr", e);
      std::string as;
      size_t after;
      if (!srcT.empty() && pp != std::string::npos &&
          matchPtrType(line, pp, as, after)) {
        size_t q = after;
        while (q < line.size() && std::isspace((unsigned char)line[q]))
          q++;
        size_t ne;
        std::string nm = readValueName(line, q, ne);
        std::string rest = line.substr(ne); // ", i64 .., .."
        auto pr = emitPtrOperand(srcT, as, nm, indent, result);
        // result type: walk aggregate indices from srcT
        if (!lhs.empty()) {
          std::vector<std::string> idxVals;
          for (auto &iv : splitTopLevel(rest)) {
            std::string s = trim(iv);
            if (!s.empty())
              idxVals.push_back(s);
          }
          cano[lhs] = typedPointer(gepResultPointee(srcT, idxVals), as);
        }
        std::string inb = inbounds ? "inbounds " : "";
        result.push_back(indent + (lhs.empty() ? "" : lhs + " = ") +
                         "getelementptr " + inb + srcT + ", " + pr.first + " " +
                         pr.second + rest);
        return;
      }
    }

    // Direct call (air intrinsic, or a helper function linked in from a
    // separately-compiled .metal module, e.g. air_tessellation.metal): bitcast
    // handle / element / global pointer arguments to what the callee expects.
    // Only direct calls (`@name(`) are handled here; indirect calls fall
    // through to the generic i8* pass below.
    if (t.find("call ") != std::string::npos) {
      size_t atp = std::string::npos;
      for (size_t search = 0; (search = line.find('@', search)) != std::string::npos; search++) {
        size_t ne;
        std::string cand = readValueName(line, search, ne);
        if (!cand.empty() && ne < line.size() && line[ne] == '(') {
          atp = search;
          break;
        }
      }
      size_t op = atp == std::string::npos ? std::string::npos : line.find('(', atp);
      std::string fn = atp == std::string::npos ? "" : funcNameOf(line, atp);
      // matching close paren
      size_t cl = std::string::npos;
      if (op != std::string::npos) {
        int depth = 0;
        for (size_t i = op; i < line.size(); i++) {
          if (line[i] == '(')
            depth++;
          else if (line[i] == ')') {
            depth--;
            if (depth == 0) {
              cl = i;
              break;
            }
          }
        }
      }
      if (op != std::string::npos && cl != std::string::npos) {
        auto args = splitTopLevel(line.substr(op + 1, cl - op - 1));
        std::string rebuilt = line.substr(0, op + 1);
        for (size_t i = 0; i < args.size(); i++) {
          std::string a = args[i];
          size_t pp = a.find("ptr");
          std::string as;
          size_t after;
          if (pp != std::string::npos && matchPtrType(a, pp, as, after)) {
            size_t lp = a.rfind('%'), latp = a.rfind('@');
            size_t pos = lp;
            if (latp != std::string::npos &&
                (pos == std::string::npos || latp > pos))
              pos = latp;
            size_t ne;
            std::string nm =
                pos == std::string::npos ? "" : readValueName(a, pos, ne);
            std::string pointee = airIntrinsicArgPointee(fn, (int)i);
            if (pointee.empty())
              pointee = "i8";
            auto pr = emitPtrOperand(pointee, as, nm, indent, result);
            a = " " + pr.first + " " + pr.second;
          }
          if (i)
            rebuilt += ",";
          rebuilt += a;
        }
        rebuilt += line.substr(cl);
        result.push_back(rebuilt);
        return;
      }
    }

    // alloca: track the result pointer type for later bitcasts.
    size_t apos = line.find(" = alloca ");
    if (apos != std::string::npos) {
      std::string lhs = trim(line.substr(0, apos));
      size_t te;
      std::string aty = readFullType(line, apos + std::strlen(" = alloca "), te);
      if (!lhs.empty() && !aty.empty())
        cano[lhs] = typedPointer(aty, "");
      result.push_back(genericI8(line));
      return;
    }

    // everything else: retype any pointer tokens to i8*. A pointer operand
    // already typed otherwise -- a GEP's result, a loaded argument field --
    // is cast to the byte pointer first; cano holds only pointers, so any
    // operand it knows is one. (Not into a phi: the cast would have to go
    // into the predecessor.)
    if (t.find(" = phi ") == std::string::npos) {
      std::string fixed = line;
      size_t eq = fixed.find(" = ");
      size_t pos = eq == std::string::npos ? 0 : eq + 3;
      while ((pos = fixed.find('%', pos)) != std::string::npos) {
        size_t ne;
        std::string nm = readValueName(fixed, pos, ne);
        if (nm.empty() || ne <= pos) {
          pos++;
          continue;
        }
        auto it = cano.find(nm);
        if (it != cano.end()) {
          std::string bytePtr = typedPointer("i8", typedPointerAddrspace(it->second));
          if (it->second != bytePtr) {
            std::string tmp = fresh();
            result.push_back(indent + tmp + " = bitcast " + it->second + " " + nm + " to " + bytePtr);
            fixed.replace(pos, ne - pos, tmp);
            ne = pos + tmp.size();
          }
        }
        pos = ne;
      }
      result.push_back(genericI8(fixed));
      return;
    }
    result.push_back(genericI8(line));
  };

  // ---- main pass: build result in order --------------------------------
  std::vector<std::string> result;
  result.reserve(lines.size() + 32);
  size_t tripleIdx = std::string::npos;

  for (size_t li = 0; li < lines.size(); li++) {
    std::string l = lines[li];
    std::string t = trim(l);

    if (tripleIdx == std::string::npos && startsWith(t, "target triple"))
      tripleIdx = result.size();

    if (startsWith(t, "define ")) {
      size_t end = li;
      while (end < lines.size() && trim(lines[end]) != "}")
        end++;
      cano.clear(); // SSA names are function-local
      result.push_back(rewriteSig(convertAttributes(l)));
      for (size_t k = li + 1; k <= end && k < lines.size(); k++) {
        std::string bl = convertAttributes(lines[k]);
        bl = expandSplat(bl);
        rewriteBody(bl, result);
      }
      li = end;
      continue;
    }

    l = convertAttributes(l);
    l = expandSplat(l);
    if (startsWith(trim(l), "declare "))
      l = rewriteSig(l);
    result.push_back(l);
  }

  // ---- metadata function-pointer typing (funcSig now complete) ---------
  for (auto &s : result) {
    std::string t = trim(s);
    if (t.empty() || t[0] != '!')
      continue;
    size_t pos = 0;
    while ((pos = s.find("ptr ", pos)) != std::string::npos) {
      size_t ap = pos + 4;
      while (ap < s.size() && std::isspace((unsigned char)s[ap]))
        ap++;
      if (ap < s.size() && s[ap] == '@') {
        size_t ne;
        std::string fn = readValueName(s, ap, ne);
        auto it = funcSig.find(fn.substr(1));
        if (it != funcSig.end()) {
          std::string repl = it->second + " " + fn;
          s.replace(pos, ne - pos, repl);
          pos += repl.size();
          continue;
        }
      }
      pos += 4;
    }
  }

  // ---- cap AIR version to what the installed Metal toolchain accepts ----
  // Keep AIR version within what the installed Metal toolchain accepts.
  {
    std::string ref; // metadata node id referenced by !air.version
    for (const auto &s : result) {
      std::string t = trim(s);
      if (startsWith(t, "!air.version")) {
        size_t b = t.find("!{");
        if (b != std::string::npos) {
          size_t e = t.find('}', b);
          ref = trim(t.substr(b + 2, e - (b + 2))); // e.g. "!9"
        }
        break;
      }
    }
    if (!ref.empty()) {
      std::string pat = ref + " = ";
      for (auto &s : result) {
        if (startsWith(trim(s), pat.c_str())) {
          // node like: !9 = !{i32 2, i32 8, i32 0}
          size_t mp = s.find("i32 2, i32 ");
          if (mp != std::string::npos) {
            size_t np = mp + std::strlen("i32 2, i32 ");
            size_t ne = np;
            while (ne < s.size() && std::isdigit((unsigned char)s[ne]))
              ne++;
            int minor = std::atoi(s.substr(np, ne - np).c_str());
            if (minor > 8)
              s = s.substr(0, np) + "8" + s.substr(ne);
          }
          break;
        }
      }
    }
  }

  // Opaque handle types referenced only as argument-buffer struct fields
  // (never as an intrinsic argument) still need a declaration.
  for (const auto &[name, fields] : argBufferFieldOverrides) {
    (void)name;
    for (const auto &f : fields) {
      if (f.rfind("%struct._", 0) == 0) {
        size_t e = f.find(' ');
        neededOpaqueStructs.insert(e == std::string::npos ? f : f.substr(0, e));
      }
    }
  }

  // ---- inject opaque struct declarations after target triple -----------
  std::vector<std::string> finalLines;
  finalLines.reserve(result.size() + 8);
  for (size_t i = 0; i < result.size(); i++) {
    finalLines.push_back(result[i]);
    if (i == tripleIdx && (!neededOpaqueStructs.empty() || !argBufferFieldOverrides.empty())) {
      finalLines.push_back("");
      for (const auto &name : neededOpaqueStructs)
        if (!definedStructs.count(name))
          finalLines.push_back(name + " = type opaque");
      for (const auto &[name, fields] : argBufferFieldOverrides) {
        if (definedStructs.count(name))
          continue;
        std::string rebuilt = name + " = type { ";
        for (size_t j = 0; j < fields.size(); j++) {
          if (j)
            rebuilt += ", ";
          rebuilt += fields[j];
        }
        rebuilt += " }";
        finalLines.push_back(rebuilt);
      }
    }
  }

  std::ostringstream os;
  for (const auto &l : finalLines)
    os << downgradeFPLiterals(l) << "\n";
  out = os.str();
  (void)err;
  return true;
}

} // namespace dxmt::aircompat
