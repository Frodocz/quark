# inplace_function

`std::function`-like callable wrapper with fixed, in-object storage: never allocates.

- Source: https://github.com/WG21-SG14/SG14/blob/master/SG14/inplace_function.h (unmodified)
- License: Boost Software License 1.0 (see the header of `inplace_function.h`)

Include it through `quark/utils/InplaceFunction.h`, which routes its `bad_function_call`
through `QUARK_THROW` so it also works with `QUARK_NO_EXCEPTIONS`.
