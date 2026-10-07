// Minimal UE3 access for Borderlands 2 (see ue3.h for the verified layout facts).
// Own implementation; byte patterns and offsets were cross-checked against our Ghidra decompile and
// documented by bl-sdk/unrealsdk (LGPL, used as a reference only - no code copied).
#include "ue3.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "log.h"

namespace bl2hdr::ue3 {

struct UObject {};
struct UFunction {};
struct FFrame {};

namespace {

template <typename T>
T At(const void* base, size_t offset) {
  return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + offset);
}

struct TArrayRaw {
  void* data;
  int32_t count;
  int32_t max;
};
struct FStringRaw {
  wchar_t* data;
  int32_t count;  // including the terminating 0
  int32_t max;
};

// ---- byte patterns (wildcards "??"), searched in the main module's code section ----
uint8_t* Scan(const char* pattern, int* hits_out = nullptr) {
  std::vector<int> bytes;  // -1 = wildcard
  for (const char* p = pattern; *p;) {
    while (*p == ' ') ++p;
    if (!*p) break;
    if (p[0] == '?') { bytes.push_back(-1); p += 2; continue; }
    bytes.push_back(static_cast<int>(strtoul(std::string(p, 2).c_str(), nullptr, 16)));
    p += 2;
  }
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  auto* sec = IMAGE_FIRST_SECTION(nt);
  uint8_t* first = nullptr;
  int hits = 0;
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    uint8_t* start = base + sec->VirtualAddress;
    const size_t size = sec->Misc.VirtualSize;
    for (size_t off = 0; off + bytes.size() <= size; ++off) {
      size_t k = 0;
      while (k < bytes.size() && (bytes[k] < 0 || start[off + k] == bytes[k])) ++k;
      if (k == bytes.size()) {
        if (!first) first = start + off;
        ++hits;
      }
    }
  }
  if (hits_out) *hits_out = hits;
  return first;
}

// ---- globals ----
TArrayRaw* g_objects = nullptr;  // TArray<UObject*>
TArrayRaw* g_names = nullptr;    // TArray<FNameEntry*>
void** g_malloc_slot = nullptr;  // address of the GMalloc pointer (FMalloc*)
std::atomic<bool> g_ready{false};
std::once_flag g_init_once;

using ProcessEvent_t = void(__fastcall*)(UObject*, void*, UFunction*, void*, void*);
using CallFunction_t = void(__fastcall*)(UObject*, void*, FFrame*, void*, UFunction*);
// UObject::StaticConstructObject(Class, Outer, FName (index, number), EObjectFlags (2 dwords), Template,
// Error, SubobjectRoot, InstanceGraph) - __cdecl, 10 dwords (verified at call sites: add esp, 0x28).
using StaticConstructObject_t = UObject*(__cdecl*)(UObject*, UObject*, int32_t, int32_t, uint32_t, uint32_t, UObject*,
                                                   void*, UObject*, void*);
StaticConstructObject_t g_construct = nullptr;
ProcessEvent_t g_process_event = nullptr;  // trampoline to the original
CallFunction_t g_call_function = nullptr;
std::atomic<ProcessEventHook> g_pe_hook{nullptr};
std::atomic<CallFunctionHook> g_cf_hook{nullptr};
thread_local bool t_in_hook = false;

// GMalloc (FMalloc vtable: [1] Malloc(len, align), [3] Free(ptr)); __thiscall via __fastcall + dummy edx.
void* UMalloc(uint32_t len) {
  void* gmalloc = *g_malloc_slot;
  if (!gmalloc) {
    log::Error("ue3: GMalloc not created yet - allocation of %u bytes refused", len);
    return nullptr;
  }
  using Malloc_t = void*(__fastcall*)(void*, void*, uint32_t, uint32_t);
  auto fn = reinterpret_cast<Malloc_t>((*reinterpret_cast<void***>(gmalloc))[1]);
  void* p = fn(gmalloc, nullptr, len, len >= 16 ? 16 : 8);
  if (p) std::memset(p, 0, len);
  return p;
}
void UFree(void* p) {
  if (!p) return;
  void* gmalloc = *g_malloc_slot;
  if (!gmalloc) return;  // UMalloc never succeeded without it
  using Free_t = void(__fastcall*)(void*, void*, void*);
  auto fn = reinterpret_cast<Free_t>((*reinterpret_cast<void***>(gmalloc))[3]);
  fn(gmalloc, nullptr, p);
}

// ---- 5-byte jump detour (both targets start with 55 8B EC 6A FF: whole instructions) ----
void* Detour(uint8_t* target, void* hook, const char* what) {
  static const uint8_t kPrologue[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
  if (std::memcmp(target, kPrologue, 5) != 0) {
    log::Error("ue3: %s prologue mismatch - not hooking", what);
    return nullptr;
  }
  auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!tramp) {
    log::Error("ue3: %s trampoline allocation failed (%lu) - not hooking", what, GetLastError());
    return nullptr;
  }
  std::memcpy(tramp, target, 5);
  tramp[5] = 0xE9;
  *reinterpret_cast<int32_t*>(tramp + 6) = static_cast<int32_t>((target + 5) - (tramp + 10));
  DWORD old = 0;
  if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) {
    log::Error("ue3: %s VirtualProtect failed (%lu) - not hooking", what, GetLastError());
    VirtualFree(tramp, 0, MEM_RELEASE);
    return nullptr;
  }
  uint8_t patch[5];
  patch[0] = 0xE9;
  *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(hook) - (target + 5));
  std::memcpy(target, patch, 5);
  VirtualProtect(target, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, 5);
  log::Info("ue3: hooked %s at %p (trampoline %p)", what, static_cast<void*>(target), static_cast<void*>(tramp));
  return tramp;
}

void __fastcall ProcessEventDetour(UObject* obj, void* edx, UFunction* fn, void* params, void* result) {
  ProcessEventHook hook = g_pe_hook.load(std::memory_order_relaxed);
  if (!hook || t_in_hook) {
    g_process_event(obj, edx, fn, params, result);
    return;
  }
  t_in_hook = true;
  const bool block = hook(obj, fn, params, false);
  t_in_hook = false;
  if (block) return;
  g_process_event(obj, edx, fn, params, result);
  t_in_hook = true;
  hook(obj, fn, params, true);
  t_in_hook = false;
}

void __fastcall CallFunctionDetour(UObject* obj, void* edx, FFrame* stack, void* result, UFunction* fn) {
  CallFunctionHook hook = g_cf_hook.load(std::memory_order_relaxed);
  if (!hook || t_in_hook) {
    g_call_function(obj, edx, stack, result, fn);
    return;
  }
  t_in_hook = true;
  hook(obj, fn, stack, false);
  t_in_hook = false;
  g_call_function(obj, edx, stack, result, fn);
  t_in_hook = true;
  hook(obj, fn, stack, true);
  t_in_hook = false;
}

std::string EntryName(int32_t index) {
  if (!g_names || index < 0 || index >= g_names->count) return "?";
  auto* entry = static_cast<uint8_t**>(g_names->data)[index];
  if (!entry) return "?";
  const int32_t flags = At<int32_t>(entry, 0x08);
  if (flags & 1) {
    const auto* w = reinterpret_cast<const wchar_t*>(entry + 0x10);
    std::string s;
    for (; *w && s.size() < 1024; ++w) s.push_back(static_cast<char>(*w < 128 ? *w : '?'));
    return s;
  }
  return std::string(reinterpret_cast<const char*>(entry + 0x10));
}

std::mutex g_find_mutex;
std::unordered_map<std::string, UObject*> g_find_cache;

}  // namespace

// ---------------- public ----------------
bool Init() {
  std::call_once(g_init_once, [] {
    const auto* exe = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    log::Info("ue3: exe timestamp %08lX, image size %08lX", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
              static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage));
    // Each pattern must match exactly as often as in the verified exe; any other count means a different
    // build, where the "right" match cannot be told apart, so nothing is hooked.
    int hits = 0;
    uint8_t* p = Scan("8B 0D ?? ?? ?? ?? 8B 04 ?? 8B 40 ?? 25 00 02 00 00", &hits);
    if (p && hits == 1) g_objects = *reinterpret_cast<TArrayRaw**>(p + 2);
    log::Info("ue3: GObjects pattern hits=%d -> TArray %p (count %d)", hits, static_cast<void*>(g_objects),
              g_objects ? g_objects->count : -1);
    p = Scan("A3 ?? ?? ?? ?? 8B 45 ?? 89 03", &hits);
    if (p && hits == 1) g_names = *reinterpret_cast<TArrayRaw**>(p + 1);
    log::Info("ue3: GNames pattern hits=%d -> TArray %p (count %d)", hits, static_cast<void*>(g_names),
              g_names ? g_names->count : -1);
    p = Scan("89 35 ?? ?? ?? ?? FF D7", &hits);
    if (p && hits == 1) g_malloc_slot = *reinterpret_cast<void***>(p + 2);
    log::Info("ue3: GMalloc pattern hits=%d -> slot %p (FMalloc %p)", hits, static_cast<void*>(g_malloc_slot),
              g_malloc_slot ? *g_malloc_slot : nullptr);
    uint8_t* pe = Scan("55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 ?? ?? ?? ?? 50 83 EC 50 A1 ?? ?? ?? ?? 33 C5 89 45 ?? "
                       "53 56 57 50 8D 45 ?? 64 A3 ?? ?? ?? ?? 8B F1", &hits);
    log::Info("ue3: ProcessEvent pattern hits=%d -> %p", hits, static_cast<void*>(pe));
    if (hits != 1) pe = nullptr;
    // Two matches in this exe; the first (UObject::CallFunction, next to ProcessEvent) is the one.
    uint8_t* cf = Scan("55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 ?? ?? ?? ?? 50 81 EC ?? ?? ?? ?? A1 ?? ?? ?? ?? 33 C5 "
                       "89 45 ?? 53 56 57 50 8D 45 ?? 64 A3 ?? ?? ?? ?? 8B 7D ?? 8B 45 ?? 8B 5D ??", &hits);
    log::Info("ue3: CallFunction pattern hits=%d -> first %p", hits, static_cast<void*>(cf));
    // Also require it to sit just before ProcessEvent (0x4F0 bytes in the verified exe).
    if (hits != 2 || !pe || cf >= pe || pe - cf > 0x1000) cf = nullptr;
    // Optional (only the HDR settings page needs it): StaticConstructObject, 0x004CA5F0 in the verified exe.
    uint8_t* sco = Scan("55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 83 EC 10 53 56 57 A1 ?? ?? ?? ?? 33 C5 50 "
                        "8D 45 F4 64 A3 00 00 00 00 8B 7D 08 8A 87 CC 01 00 00", &hits);
    log::Info("ue3: StaticConstructObject pattern hits=%d -> %p", hits, static_cast<void*>(sco));
    if (sco && hits == 1) g_construct = reinterpret_cast<StaticConstructObject_t>(sco);
    // GMalloc itself is created later by the engine: only its slot must be known now (read at use time).
    if (!g_objects || !g_names || !g_malloc_slot || !pe || !cf) {
      log::Error("ue3: initialisation incomplete - game hooks disabled");
      return;
    }
    g_process_event = reinterpret_cast<ProcessEvent_t>(Detour(pe, reinterpret_cast<void*>(&ProcessEventDetour), "ProcessEvent"));
    g_call_function = reinterpret_cast<CallFunction_t>(Detour(cf, reinterpret_cast<void*>(&CallFunctionDetour), "CallFunction"));
    if (!g_process_event || !g_call_function) return;
    g_ready = true;
  });
  return g_ready;
}

bool Ready() { return g_ready; }

void SelfCheck() {
  if (!g_ready) return;
  // The first object should be a package/class with a sane name if the layout facts are right.
  UObject* first = g_objects->count > 0 ? static_cast<UObject**>(g_objects->data)[0] : nullptr;
  log::Info("ue3: self-check: %d objects, %d names, GObjects[0] = '%s' (class '%s'), GMalloc %p", g_objects->count,
            g_names->count, PathName(first).c_str(), ClassName(first).c_str(), *g_malloc_slot);
}

int32_t ParamOffset(UFunction* fn, const char* name) {
  if (!fn) return -1;
  for (UObject* child = At<UObject*>(fn, 0x4C); child; child = At<UObject*>(child, 0x3C)) {
    if (Name(child) != name) continue;
    // Callers read the value (ElementSize bytes) from a ParamsSize-byte block: it must lie inside.
    const int32_t offset = At<int32_t>(child, 0x60);
    const int32_t size = At<int32_t>(child, 0x44);
    if (offset < 0 || size <= 0 || offset + size > At<uint16_t>(fn, 0x9E)) {
      log::Warn("ue3: %s parameter '%s' at %d(%d) lies outside the parameter block", PathName(reinterpret_cast<UObject*>(fn)).c_str(),
                name, offset, size);
      return -1;
    }
    return offset;
  }
  return -1;
}

std::string Name(const UObject* obj) {
  if (!obj) return "None";
  const int32_t index = At<int32_t>(obj, 0x2C);
  const int32_t number = At<int32_t>(obj, 0x30);
  std::string s = EntryName(index);
  if (number > 0) s += "_" + std::to_string(number - 1);
  return s;
}
UObject* Outer(const UObject* obj) { return obj ? At<UObject*>(obj, 0x28) : nullptr; }
UObject* ClassOf(const UObject* obj) { return obj ? At<UObject*>(obj, 0x34) : nullptr; }
std::string ClassName(const UObject* obj) { return Name(ClassOf(obj)); }
UObject* CallerObject(const FFrame* stack) { return stack ? At<UObject*>(stack, 0x14) : nullptr; }

std::string PathName(const UObject* obj) {
  std::string path;
  for (int depth = 0; obj && depth < 16; obj = Outer(obj), ++depth) {
    path = path.empty() ? Name(obj) : Name(obj) + "." + path;
  }
  return path;
}

UObject* FindObject(const char* class_name, const std::string& path) {
  const std::string key = std::string(class_name) + "|" + path;
  {
    std::lock_guard lock(g_find_mutex);
    if (auto it = g_find_cache.find(key); it != g_find_cache.end()) return it->second;
  }
  // Last path component first: cheap filter before building full paths.
  const std::string last = path.substr(path.find_last_of('.') + 1);
  UObject* found = nullptr;
  auto** objs = static_cast<UObject**>(g_objects->data);
  for (int32_t i = 0; i < g_objects->count && !found; ++i) {
    UObject* o = objs[i];
    if (!o || Name(o) != last || ClassName(o) != class_name) continue;
    if (PathName(o) == path) found = o;
  }
  if (found) {  // misses are not cached: the object may simply not be loaded yet
    std::lock_guard lock(g_find_mutex);
    g_find_cache[key] = found;
  }
  log::Info("ue3: FindObject %s '%s' -> %p", class_name, path.c_str(), static_cast<void*>(found));
  return found;
}

UObject* FindInstance(const char* class_name) {
  auto** objs = static_cast<UObject**>(g_objects->data);
  for (int32_t i = 0; i < g_objects->count; ++i) {
    UObject* o = objs[i];
    if (!o || ClassName(o) != class_name) continue;
    if (Name(o).rfind("Default__", 0) == 0) continue;
    return o;
  }
  return nullptr;
}

UFunction* FindFunction(const std::string& path) {
  return reinterpret_cast<UFunction*>(FindObject("Function", path));
}

// ---------------- Params ----------------
Params::Params(UFunction* fn) : fn_(fn) {
  buf_.assign(At<uint16_t>(fn, 0x9E), 0);
  for (UObject* child = At<UObject*>(fn, 0x4C); child; child = At<UObject*>(child, 0x3C)) {
    const std::string cls = ClassName(child);
    if (cls.size() < 8 || cls.compare(cls.size() - 8, 8, "Property") != 0) continue;
    const uint32_t flags = At<uint32_t>(child, 0x48);
    if (!(flags & 0x80)) continue;  // CPF_Parm
    Prop p{Name(child), cls, At<int32_t>(child, 0x60), At<int32_t>(child, 0x44) * At<int32_t>(child, 0x40), flags,
           cls == "BoolProperty" ? At<uint32_t>(child, 0x80) : 0u};
    if (p.offset < 0 || p.size <= 0 || static_cast<int64_t>(p.offset) + p.size > static_cast<int64_t>(buf_.size())) {
      log::Warn("ue3: %s parameter '%s' at %d(%d) lies outside the %zu-byte block - ignored",
                PathName(reinterpret_cast<UObject*>(fn)).c_str(), p.name.c_str(), p.offset, p.size, buf_.size());
      continue;
    }
    props_.push_back(p);
  }
}

Params::~Params() {
  for (void* p : owned_) UFree(p);
}

const Params::Prop* Params::Find(const char* name, size_t bytes) const {
  for (const auto& p : props_) {
    if (p.name != name) continue;
    if (bytes > static_cast<size_t>(p.size)) {
      log::Warn("ue3: %s parameter '%s' is %d bytes, %zu expected", PathName(reinterpret_cast<UObject*>(fn_)).c_str(),
                name, p.size, bytes);
      return nullptr;
    }
    return &p;
  }
  log::Warn("ue3: %s has no parameter '%s' (%s)", PathName(reinterpret_cast<UObject*>(fn_)).c_str(), name,
            Describe().c_str());
  return nullptr;
}

bool Params::SetInt(const char* name, int32_t v) {
  const Prop* p = Find(name, 4);
  if (!p) return false;
  std::memcpy(&buf_[p->offset], &v, 4);
  return true;
}
bool Params::SetBool(const char* name, bool v) {
  const Prop* p = Find(name, 4);
  if (!p) return false;
  uint32_t word = 0;
  std::memcpy(&word, &buf_[p->offset], 4);
  word = v ? (word | p->bool_mask) : (word & ~p->bool_mask);
  std::memcpy(&buf_[p->offset], &word, 4);
  return true;
}
bool Params::SetObject(const char* name, UObject* v) {
  const Prop* p = Find(name, sizeof(v));
  if (!p) return false;
  std::memcpy(&buf_[p->offset], &v, sizeof(v));
  return true;
}
bool Params::SetFloat(const char* name, float v) {
  const Prop* p = Find(name, sizeof(v));
  if (!p || p->cls != "FloatProperty") return false;
  std::memcpy(&buf_[p->offset], &v, sizeof(v));
  return true;
}
bool Params::SetInterface(const char* name, UObject* v) {
  const Prop* p = Find(name, 2 * sizeof(v));
  if (!p || p->cls != "InterfaceProperty") return false;
  UObject* both[2] = {v, v};
  std::memcpy(&buf_[p->offset], both, sizeof(both));
  return true;
}
bool Params::SetString(const char* name, const std::wstring& v) {
  const Prop* p = Find(name, sizeof(FStringRaw));
  if (!p) return false;
  FStringRaw s{};
  s.count = s.max = static_cast<int32_t>(v.size() + 1);
  s.data = static_cast<wchar_t*>(UMalloc(static_cast<uint32_t>(s.count * sizeof(wchar_t))));
  if (!s.data) return false;
  std::memcpy(s.data, v.c_str(), s.count * sizeof(wchar_t));
  owned_.push_back(s.data);
  std::memcpy(&buf_[p->offset], &s, sizeof(s));
  return true;
}
bool Params::SetStringArray(const char* name, const std::vector<std::wstring>& v) {
  const Prop* p = Find(name, sizeof(TArrayRaw));
  if (!p) return false;
  TArrayRaw arr{};
  arr.count = arr.max = static_cast<int32_t>(v.size());
  auto* items = static_cast<FStringRaw*>(UMalloc(static_cast<uint32_t>(v.size() * sizeof(FStringRaw))));
  if (!items) return false;
  owned_.push_back(items);
  for (size_t i = 0; i < v.size(); ++i) {
    items[i].count = items[i].max = static_cast<int32_t>(v[i].size() + 1);
    items[i].data = static_cast<wchar_t*>(UMalloc(static_cast<uint32_t>(items[i].count * sizeof(wchar_t))));
    if (!items[i].data) return false;  // already-allocated items are freed with owned_
    std::memcpy(items[i].data, v[i].c_str(), items[i].count * sizeof(wchar_t));
    owned_.push_back(items[i].data);
  }
  arr.data = items;
  std::memcpy(&buf_[p->offset], &arr, sizeof(arr));
  return true;
}
int32_t Params::GetInt(const char* name) const {
  const Prop* p = Find(name, 4);
  int32_t v = 0;
  if (p) std::memcpy(&v, &buf_[p->offset], 4);
  return v;
}
UObject* Params::GetObject(const char* name) const {
  const Prop* p = Find(name, sizeof(UObject*));
  UObject* v = nullptr;
  if (p) std::memcpy(&v, &buf_[p->offset], sizeof(v));
  return v;
}
std::wstring Params::GetString(const char* name) const {
  const Prop* p = Find(name, sizeof(FStringRaw));
  if (!p) return L"";
  FStringRaw s{};
  std::memcpy(&s, &buf_[p->offset], sizeof(s));
  return (s.data && s.count > 0) ? std::wstring(s.data, s.count - 1) : L"";
}
void Params::SetReturnBool(bool v) {
  for (const auto& p : props_) {
    if ((p.flags & 0x400) && p.size >= 4) {  // CPF_ReturnParm
      uint32_t word = v ? p.bool_mask : 0u;
      std::memcpy(&buf_[p.offset], &word, 4);
    }
  }
}
std::wstring Params::TakeReturnString() {
  for (const auto& p : props_) {
    if ((p.flags & 0x400) && p.cls == "StrProperty" && p.size >= static_cast<int32_t>(sizeof(FStringRaw))) {
      FStringRaw s{};
      std::memcpy(&s, &buf_[p.offset], sizeof(s));
      std::wstring out = (s.data && s.count > 0) ? std::wstring(s.data, s.count - 1) : L"";
      if (s.data) owned_.push_back(s.data);
      std::memset(&buf_[p.offset], 0, sizeof(s));
      return out;
    }
  }
  return L"";
}

std::string Params::Describe() const {
  std::string s;
  for (const auto& p : props_) {
    s += p.name + ":" + p.cls + "@" + std::to_string(p.offset) + "(" + std::to_string(p.size) + ") ";
  }
  return s + "size=" + std::to_string(buf_.size());
}

void Call(UObject* obj, UFunction* fn, void* params, bool hooks) {
  if (!g_process_event || !obj || !fn) return;
  const bool was = t_in_hook;
  t_in_hook = !hooks;  // our own calls do not re-enter our hooks unless asked
  g_process_event(obj, nullptr, fn, params, nullptr);
  t_in_hook = was;
}

void SetProcessEventHook(ProcessEventHook hook) { g_pe_hook = hook; }
void SetCallFunctionHook(CallFunctionHook hook) { g_cf_hook = hook; }

namespace {
// The property `prop_name` of obj's class (or a superclass), or nullptr.
UObject* FindMember(UObject* obj, const char* prop_name) {
  for (UObject* cls = ClassOf(obj); cls; cls = At<UObject*>(cls, 0x48)) {  // UStruct::SuperField
    for (UObject* child = At<UObject*>(cls, 0x4C); child; child = At<UObject*>(child, 0x3C)) {
      if (Name(child) == prop_name) return child;
    }
  }
  return nullptr;
}
}  // namespace

bool ReadIntArray(UObject* obj, const char* prop_name, std::vector<int32_t>* out) {
  UObject* prop = obj ? FindMember(obj, prop_name) : nullptr;
  if (!prop) return false;
  const auto* arr = reinterpret_cast<const TArrayRaw*>(reinterpret_cast<const uint8_t*>(obj) + At<int32_t>(prop, 0x60));
  if (arr->count < 0 || arr->count > arr->max || (arr->count > 0 && !arr->data)) return false;
  out->assign(static_cast<const int32_t*>(arr->data), static_cast<const int32_t*>(arr->data) + arr->count);
  return true;
}

bool GetObjectMember(UObject* obj, const char* prop_name, UObject** out) {
  UObject* prop = obj ? FindMember(obj, prop_name) : nullptr;
  if (!prop || ClassName(prop) != "ObjectProperty") return false;
  *out = At<UObject*>(obj, At<int32_t>(prop, 0x60));
  return true;
}

bool SetObjectMember(UObject* obj, const char* prop_name, UObject* value) {
  UObject* prop = obj ? FindMember(obj, prop_name) : nullptr;
  if (!prop || ClassName(prop) != "ObjectProperty") return false;
  *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(obj) + At<int32_t>(prop, 0x60)) = value;
  return true;
}

bool SetStringMember(UObject* obj, const char* prop_name, const std::wstring& value) {
  UObject* prop = obj ? FindMember(obj, prop_name) : nullptr;
  if (!prop || ClassName(prop) != "StrProperty") return false;
  auto* s = reinterpret_cast<FStringRaw*>(reinterpret_cast<uint8_t*>(obj) + At<int32_t>(prop, 0x60));
  const auto count = static_cast<int32_t>(value.size() + 1);
  auto* data = static_cast<wchar_t*>(UMalloc(static_cast<uint32_t>(count * sizeof(wchar_t))));
  if (!data) return false;
  std::memcpy(data, value.c_str(), count * sizeof(wchar_t));
  UFree(s->data);
  s->data = data;
  s->count = s->max = count;
  return true;
}

bool CanConstruct() { return g_ready && g_construct; }

UObject* Construct(UObject* cls, UObject* outer) {
  if (!CanConstruct() || !cls || !outer || ClassName(cls) != "Class") return nullptr;
  if (At<uint32_t>(cls, 0xD0) & 1u) {  // UClass::ClassFlags, CLASS_Abstract
    log::Error("ue3: Construct: class '%s' is abstract", Name(cls).c_str());
    return nullptr;
  }
  // Error device null: only used for failures (abstract class, wrong outer), excluded above.
  UObject* obj = g_construct(cls, outer, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
  log::Info("ue3: constructed %s '%s' in '%s'", Name(cls).c_str(), PathName(obj).c_str(), PathName(outer).c_str());
  return obj;
}

}  // namespace bl2hdr::ue3
