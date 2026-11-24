#ifndef PLATFORM_CONFIG_H_
#define PLATFORM_CONFIG_H_

namespace pf
{

// Optional key=value settings file; missing file or key means the default. Lines starting with # are comments.
// Invalid lines and values are logged and ignored; a key that is not a known setting is logged once and ignored.
class Config
{
public:
    static void load(const char* path);
    // False if the key is missing or its value invalid (logged); value is then unchanged.
    // Booleans: on/off, true/false, yes/no, 1/0 in any case. Integers: decimal.
    static bool tryGetBool(const char* key, bool* value);
    static bool tryGetInt(const char* key, int* value);
    static bool getBool(const char* key, bool defaultValue);
    static int  getInt(const char* key, int defaultValue);
    static const char* getString(const char* key, const char* defaultValue);
    // Index of the value in names[0..count) (any case); defaultIndex if the key is missing or the value is not one
    // of the names (logged).
    static int  getChoice(const char* key, const char* const* names, int count, int defaultIndex);
};

// A setting for hot code, read on first use. A function-local `static const x = Config::get...()` pays a thread-safe
// guard check (a load and a memory barrier) on every call: ~2 % of the game thread at the barriers (run 392). These
// are constant-initialized, so a call is a plain load. Two threads reading one first both store the same value.
struct LazyBool
{
    signed char state = -1;
    bool get(const char* key, bool defaultValue)
    {
        if (state < 0)
            state = Config::getBool(key, defaultValue) ? 1 : 0;
        return state != 0;
    }
};

struct LazyInt
{
    static constexpr int Unread = int(0x80000000u);
    int value = Unread;
    int get(const char* key, int defaultValue)
    {
        if (value == Unread)
            value = Config::getInt(key, defaultValue);
        return value;
    }
};

}

#endif
