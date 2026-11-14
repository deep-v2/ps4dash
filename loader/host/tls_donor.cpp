// TLS donor DLL (Windows host only). A manually mapped image cannot register static
// TLS with ntdll, so we load one copy of this DLL per mapped image: ntdll gives it a
// real TLS slot and allocates/grows/frees it for every thread. The host copies the
// image's TLS template over this DLL's template and points the image's _tls_index here.
#include <windows.h>
extern "C" {
// Non-zero initializer keeps the block in .tls raw data (a template ntdll copies from).
__declspec(thread) unsigned char gdps4_tls_block[64 * 1024] = {1};
extern ULONG _tls_index;
__declspec(dllexport) ULONG gdps4_tls_slot() { return _tls_index; }
__declspec(dllexport) void* gdps4_tls_this_thread() { return gdps4_tls_block; }
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
