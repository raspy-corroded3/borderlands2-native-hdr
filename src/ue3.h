#pragma once
// Minimal Unreal Engine 3 (Borderlands 2, 32-bit) access for milestone 5: objects, names, function
// lookup, parameter blocks, calls, and hooks on ProcessEvent / CallFunction.
// Layout facts (verified against the game binary and in the running game):
//   UObject:  Outer +0x28, Name (FName: index, number) +0x2C, Class +0x34
//   UField:   Next +0x3C           UStruct: Children +0x4C
//   UFunction: FunctionFlags +0x8C, iNative +0x90, ParamsSize +0x9E, ReturnValueOffset +0xA0
//   UProperty: ArrayDim +0x40, ElementSize +0x44, PropertyFlags +0x48, Offset +0x60
//   FNameEntry: flags/index +0x08 (bit 0 = wide), name text +0x10
//   FFrame:   Node +0x10, Object +0x14, Code +0x18
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bl2hdr::ue3 {
struct UObject;
struct UFunction;
struct FFrame;

// Find globals/functions by byte pattern and install the ProcessEvent/CallFunction detours. Safe to
// call from DllMain (code patterns and global addresses are static); idempotent; logs everything.
bool Init();
bool Ready();
void SelfCheck();  // after the engine started (first Present): logs GObjects[0], object/name counts

// Offset of a parameter in a function's parameter block (-1 if not found).
int32_t ParamOffset(UFunction* fn, const char* name);

std::string Name(const UObject* obj);       // object name ("None" if null)
std::string ClassName(const UObject* obj);  // name of the object's class
std::string PathName(const UObject* obj);   // outer chain joined with '.'
UObject* Outer(const UObject* obj);
UObject* ClassOf(const UObject* obj);
UObject* CallerObject(const FFrame* stack);  // FFrame::Object

// Find an object by class name and full path ("WillowGame.WillowScrollingList.AddSpinnerListItem").
UObject* FindObject(const char* class_name, const std::string& path);
// First non-default instance whose class is (or is named) class_name.
UObject* FindInstance(const char* class_name);
UFunction* FindFunction(const std::string& path);

// A parameter block for one UFunction, filled by parameter name (offsets read from the function).
class Params {
 public:
  explicit Params(UFunction* fn);
  ~Params();  // frees strings/arrays we allocated
  void* data() { return buf_.data(); }
  bool SetInt(const char* name, int32_t v);
  bool SetBool(const char* name, bool v);
  bool SetObject(const char* name, UObject* v);
  bool SetString(const char* name, const std::wstring& v);
  bool SetStringArray(const char* name, const std::vector<std::wstring>& v);
  int32_t GetInt(const char* name) const;
  UObject* GetObject(const char* name) const;
  std::wstring GetString(const char* name) const;
  void SetReturnBool(bool v);
  std::wstring TakeReturnString();  // read the returned FString and free it (engine-allocated)
  std::string Describe() const;  // "name@offset(size) ..." for logging
 private:
  struct Prop { std::string name, cls; int32_t offset, size; uint32_t flags; uint32_t bool_mask; };
  const Prop* Find(const char* name) const;
  UFunction* fn_;
  std::vector<uint8_t> buf_;
  std::vector<Prop> props_;
  std::vector<void*> owned_;  // GMalloc allocations to free
};

// Call a UnrealScript/native function on obj through the original ProcessEvent.
void Call(UObject* obj, UFunction* fn, void* params);

// Hooks. Return true from a pre-hook to block the original call.
using ProcessEventHook = bool (*)(UObject* obj, UFunction* fn, void* params, bool post);
using CallFunctionHook = void (*)(UObject* obj, UFunction* fn, FFrame* stack, bool post);
void SetProcessEventHook(ProcessEventHook hook);
void SetCallFunctionHook(CallFunctionHook hook);

// Read an int array member (TArray<int>) of an object by property name; returns false if not found.
bool ReadIntArray(UObject* obj, const char* prop_name, std::vector<int32_t>* out);
}  // namespace bl2hdr::ue3
