## General
- DO NOT modify files unless explicitly asked. If a problem/error is pasted, EXPLAIN how it came about, not FIX IT. Present your findings, I will decide whether or not you should fix it.
- DO NOT USE UTILITY FUNCTIONS IF ITS BODY CAN BE REPLICATED IN A FEW STATEMENTS.
- DO NOT MODIFY AGENTS.md UNLESS EXPLICITLY STATED.
- Push Ifs Up And Fors Down. If you have complex control flow, better to fit it on a screen in a single function, rather than spread throughout the file.
- When we engage in exploratory back-and-forth conversational questioning, keep your answers concise so you don't bloat the context window.
- Avoid using line continuations for shell script documentation or code. It's easier to copy paste.
- Don't add tests unless explicitly asked for it.
- Files are written in such a way that each successive file only depends on files that come before it. This makes it easy to rewrite the codebase from scratch file-by-file.
- DO NOT use short form variable names for longer type names. If a type is ProviderOptions, the variable _SHOULD_ be providerOptions, NOT options. If the variable name is too long, come up with a shorter type name. This rule helps prevent future variable name collisions.

## C++
- Follow .clang-format.
- Always run ./scripts/lint.ps1 -changed -quiet when source files were added or changed.
- Use ./scripts/build.ps1 -quiet to build the project quietly and not pollute the context window.
- Write C++ like Go: prefer straightforward conditionals and loops, useful zero values, and structs/classes.
- Use std types where possible.
- Prefer adding minimal structs and fields.
- DO NOT use auto when the type can be explicitly named EXCEPT for lambdas, so we don't have to step through a std::function wrapper during debugging.
- DO NOT use *T t = new T, use *T t = new T().
- DO NOT use uniform initialization syntax i.e. int64_t num{0}.
    1. Prefer assigning literals directly std::vector<int> v = {1, 2, 3}. This applies to default member initializers also.
    2. If calling a default constructor, just omit the parens entirely i.e. T t not T t{}.
    3. Use traditional constructor syntax for parameterized constructors i.e. T t(a, b).
    4. If a constructor argument is a constructor call, wrap it in parentheses e.g. (A()) to disambiguate it from the Most Vexing Parse.
- Every struct data member MUST have an explicit default member initializer, even when omitting it would produce identical behavior. Use = {}; for containers, = ""; for strings, = nullptr; for pointers, and the appropriate zero value for scalars. Never omit an initializer merely because the type default-constructs itself. Before completing a struct change, check every field for compliance.
    - Default member initializers should default to the type's zero value, like Go. It is up to the callers to set the fields to any other meaningful value besides the zero value.
- Prefer struct designated initializers (aka struct literals) over struct constructors.
    - Prefer struct literals if it means we can avoid declaring a variable.
    - Do NOT define designated initializers in one line. EACH MEMBER SHOULD BE IN ITS OWN LINE.
        - /* BAD */ throw detail::Failure{.error = detail::portableError("InvalidResult", "Invalid iterator cleanup result")};
        - /* GOOD */ throw detail::Failure{
            .error = detail::portableError("InvalidResult", "Invalid iterator cleanup result"),
        }
- DO NOT use underscore suffixes or m_ prefixes to denote member fields. Just use normal names. If a private field and a public getter method would clash, just use xxx and getXXX().
- DO NOT use default arguments. Be pedantic that callers should supply ALL arguments. Just like in Go.
- DO NOT use implicit bool <-> int conversions.
- Use const BEFORE type i.e. west const.
- Floating point literals MUST always specify both the integer part and fractional part i.e. 0.5.
- Explicitly handle division by zero with an if check.
- Don't use unsigned ints if you can help it. Avoid size_t. Prefer int64_t. The only exceptions are when working with standard library functions, in which case we should be as idiomatic as possible.
- We are not writing high performance code, readability is more important:
    - Do not use struct of arrays if array of structs is more natural.
    - Semantic order of class members is more important than struct packing.
    - Do NOT use #ifdef. Use only #if defined().
- Add NOLINT(bugprone-throwing-static-initialization) to globals whose only potential initialization failure is allocation failure; startup termination is acceptable. Account for Windows STL allocations even when macOS lint passes.
- NEVER mix platform-specific implementations in one file. Put them in separate \*\_windows.cpp, \*\_darwin.cpp, and \*\_linux.cpp files. Guard each ENTIRE file, including all includes, with #if defined(_WIN32), #if defined(__APPLE__), or #if defined(__linux__), respectively.
- Do NOT use #ifdef. Use only #if defined().
- For Apple, highly prefer the internal headers (sys/fcntl.h) over the portable ones (fcntl.h) to placate the clangd linter.
- Never implicitly convert between integers and bools. Test integers with == 0 / != 0; test bools directly or with !. Class types with explicit operator bool() (e.g. std::error_code) may be tested directly.

## Python
- Use typed Python as much as possible.
- Write Python like Go: prefer straightforward conditionals and loops, useful zero values, and dataclasses for structs.
- DO NOT use list comprehensions.
- DO NOT use `from foo.bar import Baz`; import the module and fully qualify names `import foo; a = foo.bar.Baz()`.
- DO NOT use vanilla python or pip, only use uv. Give the user the command(s) that installs uv if uv is not available.
- DO NOT use python function scoping, treat it as though Python only supports strict block-level lexical scoping.
- DO NOT use python ternary expressions. Declare a default value first if possible, the overwrite it if some condition holds true.
- Before completing Python changes, run `uv run ruff format .`, `uv run ruff check --fix .` and `uv run pyright`.

## SQL
- Table names should use singular form, not plural form.
- Never use the bare "id" by itself e.g. use "document_id" for a "document" table.
- Prefer UUIDv7 over autoincrement for primary keys. Either generate it application-side or use uuidv7() builtins.
- Use Postgres-naming convention for constraint/index names.
- For schema migrations, strictly use a roll-forward only migration strategy. Never do a down migration.

## C++ Safety Rules

Prioritize memory safety, defined behavior, and predictable failure over micro-optimizations. Prefer checked operations even when they incur runtime overhead.

### Checked access
- Use `.at(index)` instead of `operator[]` for vectors, arrays, strings, and other containers providing checked indexing.
- Use `std::span::at()` when available (C++26); otherwise explicitly validate indices before indexing spans.
- Use `.value()` instead of `*` or `->` when accessing `std::optional` and `std::expected` values.
- Use `.at(key)` for map lookups where the key must exist. Use `.find()` or `.contains()` where absence is expected. Use `operator[]` only when insertion is intentional.
- Use `std::get_if` instead of `std::get` for variants when a missing alternative should be handled without exceptions.
- Prefer pointer-returning `std::any_cast` when an incorrect type is an expected possibility.
- Check `.empty()` before calling `.front()`, `.back()`, or `.pop_back()`.

### Memory and lifetime safety
- Prefer stack allocation, RAII, standard containers, and smart pointers.
- Avoid explicit `new`, `delete`, `malloc`, and `free`.
- Prefer `std::unique_ptr` for dynamic ownership; use `std::shared_ptr` only when shared ownership is necessary.
- Prefer references for required borrowed objects and pointers for nullable borrowed objects.
- Never dereference a pointer unless its validity is established.
- Never dereference end iterators or use invalidated iterators.
- Never retain references, pointers, spans, string_views, or iterators beyond the lifetime or validity of their underlying storage.
- Account for iterator, pointer, and reference invalidation whenever containers are mutated.
- Prefer typed containers and algorithms over raw pointer arithmetic and byte manipulation.
- Avoid `reinterpret_cast`, C-style casts, and unions for type punning.
- Use `std::bit_cast` where its requirements are satisfied.
- Avoid returning references or pointers to local objects.
- Do not use `std::move` on objects that must retain their original value.

### Arithmetic safety
- Validate divisors before integer division or modulo; handle signed minimum divided by -1.
- Prevent signed integer overflow and unsigned wraparound unless explicitly intended.
- Use checked arithmetic for values that may exceed their type's range.
- Validate numeric ranges before narrowing conversions.
- Validate shift counts and signedness before bit shifts.
- Prefer `std::from_chars` or explicitly checked conversions over `atoi`/`atof`.

### C APIs and buffer handling
- Prefer `std::string`, `std::vector`, `std::array`, and `std::span` over C-style buffers.
- Avoid unbounded C string functions such as `strcpy`, `strcat`, and `sprintf`.
- When using C APIs, explicitly validate buffer capacities, null termination, ownership, and lifetimes.
- Do not assume `std::copy`, `memcpy`, or `memmove` automatically checks buffer bounds.

### Exceptions and error handling
- Prefer checked operations that throw or return explicit errors over unchecked operations with undefined behavior.
- Handle expected failures explicitly.
- Never use `assert()` as the only validation for inputs or conditions that can fail in release builds.
- Do not suppress exceptions or errors merely to allow execution to continue.

### Performance exceptions
- Do not replace checked operations with unchecked alternatives solely for presumed performance.
- Permit unchecked operations only where invariants make them provably safe and a measured performance benefit justifies them.
- Document the relevant invariant and its enforcement whenever an unchecked operation is intentionally used.
