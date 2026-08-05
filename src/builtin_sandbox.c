#include "lisp.h"
#include <string.h>

/* (io-sandbox-allow! pattern) */
int builtin_io_sandbox_allow(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "io-sandbox-allow! expects a string pattern");
	}
	io_sandbox_allow_path(car(args).value.string);
	*result = make_sym("T");
	return Error_OK;
}

/* (io-sandbox-deny! pattern) */
int builtin_io_sandbox_deny(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "io-sandbox-deny! expects a string pattern");
	}
	io_sandbox_deny_path(car(args).value.string);
	*result = make_sym("T");
	return Error_OK;
}

/* (io-sandbox-mode [mode]) - get or set mode */
int builtin_io_sandbox_mode(Atom args, Atom *result) {
	if (nilp(args)) {
		/* Get mode */
		const char *mode_str;
		switch (io_sandbox_get_mode()) {
		case IO_SANDBOX_UNRESTRICTED: mode_str = "UNRESTRICTED"; break;
		case IO_SANDBOX_RESTRICTED: 
			mode_str = io_sandbox_get_default() ? "RESTRICTED-ALLOW" : "RESTRICTED-DENY"; 
			break;
		case IO_SANDBOX_DISABLED: mode_str = "DISABLED"; break;
		default: mode_str = "UNKNOWN"; break;
		}
		*result = make_sym(mode_str);
	} else {
		/* Set mode */
		Atom mode_arg = car(args);
		if (mode_arg.type != AtomType_Symbol) {
			return raise_error("type-error", "io-sandbox-mode expects a symbol");
		}
		const char *s = mode_arg.value.symbol;
		if (strcmp(s, "UNRESTRICTED") == 0) {
			io_sandbox_init(IO_SANDBOX_UNRESTRICTED);
		} else if (strcmp(s, "RESTRICTED") == 0 || strcmp(s, "RESTRICTED-ALLOW") == 0) {
			io_sandbox_init(IO_SANDBOX_RESTRICTED);
			io_sandbox_set_default(1); /* Allow by default */
		} else if (strcmp(s, "RESTRICTED-DENY") == 0) {
			io_sandbox_init(IO_SANDBOX_RESTRICTED);
			io_sandbox_set_default(0); /* Deny by default */
		} else if (strcmp(s, "DISABLED") == 0) {
			io_sandbox_init(IO_SANDBOX_DISABLED);
		} else {
			return raise_error("value-error", "unknown sandbox mode (use unrestricted, restricted, restricted-allow, restricted-deny, or disabled)");
		}
		*result = mode_arg;
	}
	return Error_OK;
}
