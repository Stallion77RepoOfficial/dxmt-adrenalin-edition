/* Exercise waiter expiry racing with single/all wakeups in the bundled Wine. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef LONG (WINAPI *wait_fn)(const void *, const void *, SIZE_T, const LARGE_INTEGER *);
typedef void (WINAPI *wake_fn)(const void *);
static wait_fn wait_address;
static wake_fn wake_one, wake_all;
static volatile LONG value, completed, errors;

static DWORD WINAPI waiter(void *argument)
{
    unsigned i, seed = (unsigned)(ULONG_PTR)argument;
    for (i = 0; i < 20000; ++i)
    {
        LONG expected = InterlockedCompareExchange(&value, 0, 0);
        LARGE_INTEGER timeout;
        LONG result;
        timeout.QuadPart = ((i + seed) % 16) ? 0 : -10000;
        result = wait_address((const void *)&value, &expected, sizeof(expected), &timeout);
        if (result != 0 && result != 0x102) InterlockedIncrement(&errors);
        InterlockedIncrement(&completed);
    }
    return 0;
}

static DWORD WINAPI waker(void *argument)
{
    unsigned i;
    (void)argument;
    for (i = 0; i < 250000; ++i)
    {
        InterlockedIncrement(&value);
        if (i & 1) wake_one((const void *)&value);
        else wake_all((const void *)&value);
        if (!(i % 256)) SwitchToThread();
    }
    return 0;
}

int main(void)
{
    HANDLE threads[10];
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    DWORD result;
    unsigned i;
    wait_address = (wait_fn)(void *)GetProcAddress(ntdll, "RtlWaitOnAddress");
    wake_one = (wake_fn)(void *)GetProcAddress(ntdll, "RtlWakeAddressSingle");
    wake_all = (wake_fn)(void *)GetProcAddress(ntdll, "RtlWakeAddressAll");
    if (!wait_address || !wake_one || !wake_all) return 2;
    for (i = 0; i < 8; ++i) threads[i] = CreateThread(NULL, 0, waiter, (void *)(ULONG_PTR)i, 0, NULL);
    for (; i < 10; ++i) threads[i] = CreateThread(NULL, 0, waker, NULL, 0, NULL);
    for (i = 0; i < 10; ++i) if (!threads[i]) return 3;
    result = WaitForMultipleObjects(10, threads, TRUE, 30000);
    printf("SYNC completed=%ld expected=160000 errors=%ld wait_result=%lu\n", completed, errors, result);
    for (i = 0; i < 10; ++i) CloseHandle(threads[i]);
    return result == WAIT_OBJECT_0 && completed == 160000 && !errors ? 0 : 1;
}
