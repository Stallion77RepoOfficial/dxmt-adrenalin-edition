#include "air_compat_text_writer.hpp"

#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
    std::vector<std::string> fields;
    size_t p = 0;
    while ((p = info.find("!\"", p)) != std::string::npos) {
      p += 2;
      size_t e = info.find('"', p);
      if (e == std::string::npos)
        break;
      std::string typeName = info.substr(p, e - p);
      std::string fieldType = airArgTypeToLLVMFieldType(typeName);
      if (!fieldType.empty())
        fields.push_back(fieldType);
      p = e + 1;
      size_t next = info.find("!\"air.indirect_argument\"", p);
      if (next == std::string::npos)
        break;
      p = next + 1;
    }
    if (!fields.empty())
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
    std::string o;
    size_t i = 0;
    while (i < s.size()) {
      std::string as;
      size_t after;
      if (matchPtrType(s, i, as, after)) {
        o += typedPointer("i8", as);
        i = after;
        continue;
      }
      o += s[i];
      i++;
    }
    return o;
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
    if (nm.empty() || nm[0] == '@')
      return {typ, nm}; // globals carry their declared type
    auto it = cano.find(nm);
    std::string from = it != cano.end() ? it->second : typedPointer("i8", as);
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
          if (!airFn) {
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
            result.push_back(indent + lhs + " = load " + (*fields)[0] +
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
              result.push_back(indent + lhs + " = load " + (*fields)[0] +
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
        result.push_back(indent + lhs + " = load " + accType + ", " + pr.first +
                         " " + pr.second + rest);
        return;
      }
    }

    // store: store VT val, ptr<as> %p <rest>
    if (startsWith(t, "store ")) {
      size_t sp = line.find("store ");
      size_t p = sp + std::strlen("store ");
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
        result.push_back(indent + "store " + vType + valText + ", " + pr.first +
                         " " + pr.second + rest);
        return;
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

    // air intrinsic call: bitcast handle / element pointer arguments.
    if (t.find("call ") != std::string::npos &&
        t.find("@air.") != std::string::npos) {
      size_t atp = line.find("@air.");
      std::string fn = funcNameOf(line, atp - 1);
      size_t op = line.find('(', atp);
      // matching close paren
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

    // everything else: retype any pointer tokens to i8*.
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
    os << l << "\n";
  out = os.str();
  (void)err;
  return true;
}

} // namespace dxmt::aircompat
