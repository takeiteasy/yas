#include "yas.h"
#include <string.h>

/* (io-sandbox-allow! pattern) */
static int yas_builtin_io_sandbox_allow_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
        return yas_raise_error("type-error", "io-sandbox-allow! expects a string pattern");
    }
    yas_io_sandbox_allow_path(yas_car(args).value.string);
    *result = yas_make_sym("T");
    return YAS_ERROR_OK;
}

int yas_builtin_io_sandbox_allow(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_io_sandbox_allow_impl(args, result);
    return r;
}


/* (io-sandbox-deny! pattern) */
static int yas_builtin_io_sandbox_deny_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
        return yas_raise_error("type-error", "io-sandbox-deny! expects a string pattern");
    }
    yas_io_sandbox_deny_path(yas_car(args).value.string);
    *result = yas_make_sym("T");
    return YAS_ERROR_OK;
}

int yas_builtin_io_sandbox_deny(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_io_sandbox_deny_impl(args, result);
    return r;
}


/* (io-sandbox-mode [mode]) - get or set mode */
static int yas_builtin_io_sandbox_mode_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args)) {
        /* Get mode */
        const char *mode_str;
        switch (yas_io_sandbox_get_mode()) {
            case YAS_IO_SANDBOX_UNRESTRICTED:
                mode_str = "UNRESTRICTED";
                break;
            case YAS_IO_SANDBOX_RESTRICTED:
                mode_str = yas_io_sandbox_get_default() ? "RESTRICTED-ALLOW" : "RESTRICTED-DENY";
                break;
            case YAS_IO_SANDBOX_DISABLED:
                mode_str = "DISABLED";
                break;
            default:
                mode_str = "UNKNOWN";
                break;
        }
        *result = yas_make_sym(mode_str);
    } else {
        /* Set mode */
        YasAtom mode_arg = yas_car(args);
        if (mode_arg.type != YAS_ATOM_TYPE_SYMBOL) {
            return yas_raise_error("type-error", "io-sandbox-mode expects a symbol");
        }
        const char *s = mode_arg.value.symbol;
        if (strcmp(s, "UNRESTRICTED") == 0) {
            yas_io_sandbox_init(YAS_IO_SANDBOX_UNRESTRICTED);
        } else if (strcmp(s, "RESTRICTED") == 0 || strcmp(s, "RESTRICTED-ALLOW") == 0) {
            yas_io_sandbox_init(YAS_IO_SANDBOX_RESTRICTED);
            yas_io_sandbox_set_default(1); /* Allow by default */
        } else if (strcmp(s, "RESTRICTED-DENY") == 0) {
            yas_io_sandbox_init(YAS_IO_SANDBOX_RESTRICTED);
            yas_io_sandbox_set_default(0); /* Deny by default */
        } else if (strcmp(s, "DISABLED") == 0) {
            yas_io_sandbox_init(YAS_IO_SANDBOX_DISABLED);
        } else {
            return yas_raise_error(
                "value-error",
                "unknown sandbox mode (use unrestricted, restricted, restricted-allow, restricted-deny, or disabled)");
        }
        *result = mode_arg;
    }
    return YAS_ERROR_OK;
}

int yas_builtin_io_sandbox_mode(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_io_sandbox_mode_impl(args, result);
    return r;
}

