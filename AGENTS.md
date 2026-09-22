# M5 Emulator Development Rules

## Code quality

- Choose the simplest, shortest approach that is correct and clear, without speculative abstractions, wrappers or duplication.
- Names and structure should make the code self-explanatory. Comment only on non-obvious intent or constraints; keep comments concise and never restate the code.
- Do not trade runtime performance for shorter or cleaner-looking code.
- Prefer language and library features available in C++26 when they improve clarity, safety or performance.
- Split or merge files, types and functions only when the result is easier to understand or change, never to meet a length target.

## Applying and reviewing these rules

- Apply these rules while coding, not just before committing.
- Before reporting completion, review the actual diff and the call chains it affects against these rules, even without a planned commit.
- Before committing, confirm the review covers the final staged diff. If code changed after review, check those changes and their impact; do not repeat unchanged reviews or rescan unrelated code.

## File organization and decomposition

- At roughly 400 lines per source file or 80 lines per function, review whether responsibilities or operations have become mixed. These are review prompts, not hard limits: a cohesive algorithm, clear type dispatch, templates, data tables and generated code may remain longer, and formatting is never compressed to meet a threshold.
- Small public interfaces and focused components are legitimate files, however short.
- Split one type's implementation by responsibility into `Type.Responsibility.cpp` files when useful. Add a type only for a distinct responsibility, state or invariant.
- Keep reusable components in the common component directory and specialized components near their users; do not add directory levels for appearance alone.
- Do not introduce context objects, state copies, public internals or single-use forwarding layers merely to connect split pieces.

## Naming and namespaces

- Use `PascalCase` for types, concepts, enumerators and named constants; `camelCase` for functions, parameters, locals and public fields; `_camelCase` for private instance data.
- Include units or distinguishing meaning when needed, such as `RetryDelayMs` or `connectionId`. Avoid vague names when a specific name helps; `value` or `data` is acceptable in a small, unambiguous scope.
- Use PascalCase `.hpp` / `.cpp` filenames for project-owned C++ code; follow existing directory conventions and retain names required by external interfaces.
- Do not use anonymous namespaces. Use `static` for `.cpp`-local functions and constants; use appropriate `inline`, `constexpr` or template linkage for header definitions.
- Prefer `namespace a::b {` unless the outer namespace has declarations of its own. Keep contents unindented and close with `} // namespace name`. A header may add a using-directive for another project-owned namespace inside its own.
- Qualify standard-library names with `std::`; do not write `using namespace std;`. Narrow using-declarations and literal namespaces may be used where they improve readability.

## Headers and includes

- Place `#pragma once` after the license header.
- Include the corresponding header first in a `.cpp`. Use quoted relative paths for nearby files and established include roots across modules.
- Group other includes by origin and role with one blank line; preserve meaningful groups and required dependency order.
- Include dependencies directly or through established common headers, not incidental transitive includes. Shared headers must remain usable by both the QEMU and frontend code.
- Do not change third-party or generated code merely to match project style.

## Types, UTF-8 and numeric expressions

- Use explicit types for obvious booleans, integers, floating-point values and characters, and `const` for unchanged locals. Allow `auto` for iterators, lambdas, generic results, complex types whose repetition adds no clarity, or when the expression makes the result type clear.
- Use UTF-8 for user-visible text and preserve the encoding across SDL, filesystem and platform API boundaries. Avoid unnecessary copying or transcoding.
- A string view's `data()` need not be null-terminated; verify lifetime and termination before passing it to a C-string API.
- Calculate in an appropriate intermediate type and narrow only where required. Avoid redundant casts; never use a cast to hide a range error.
- Write hexadecimal digits in uppercase.
- Preserve required floating-point precision and choose matching overloads to avoid accidental costly promotion.

## C++ safety, ownership and borrowing

- Prefer type-safe C++ without unnecessary runtime cost. Use `array` for fixed storage, clear types, `enum class` and compile-time constraints where appropriate.
- Pass complex, owning or larger-than-8-byte objects by `const T&` for reading, `T&` for modification and `T&&` for explicit ownership transfer. Do not default to taking an object by value and then moving it.
- Use `string_view` / `u8string_view` for borrowed text and `span` for borrowed contiguous data. These lightweight non-owning descriptors may be passed by value. Store an owning value unless the borrowed storage is guaranteed to remain alive and valid.
- Prefer direct member ownership and manage resources through RAII. Use `unique_ptr` only for dynamic exclusive ownership and `shared_ptr` only when independent owners genuinely share a lifetime; avoid ownership cycles. Pure borrowing takes references or views, not smart pointers.
- Prefer non-null references to raw pointers and `span` to pointer-and-length pairs. Use raw pointers for nullable borrowing or where C API, hardware, DMA or SIMD boundaries require them; keep ownership explicit. Do not add smart pointers, wrappers, allocations or redundant runtime checks merely to replace necessary low-level code.
- Compare raw pointers explicitly with `nullptr`; test smart pointers and `optional` directly as conditions.
- C++ types alone do not prove safety: verify access bounds and borrowing lifetimes through the full call chain, from actual arguments and parameter passing to member storage and release. Every stored reference or view must have an owner that outlives all its uses; check replacement, clearing, destruction order, container reallocation, copying and moving. For stored callbacks, also check captures, cancellation and the lifetime of `this`. Remove unnecessary copies, moves and allocations without introducing lifetime risks.
- Copy and move operations must match the type's semantics. Value types may legitimately copy their strings and containers; resource handles and identity-bearing objects must not allow accidental copying. Default operations are appropriate only when ownership and borrowed members remain correct. `std::move` alone does not prove that a move occurs.
- Parameter rules do not forbid returning a new result by value. Return it directly rather than adding an output parameter, a dangling reference or an unnecessary `std::move` of a local result.
- Lifetime safety and thread safety are separate. `shared_ptr` does not synchronize access to the object, and `const` does not establish cross-thread synchronization. Make the synchronization of shared mutable state explicit.
- Define the state left by a fallible operation: unchanged, cleared or partially updated must match its contract. Do not mark a cache or operation successful before the required work succeeds.
- Validate external inputs and necessary boundaries. Do not repeat checks at every layer for conditions already guaranteed by types or established invariants.
- Do not reorder serialized fields, renumber enumerators or change hardware and external layouts merely for style. Keep the shared-memory and BLE transport layouts consistent between the frontend and QEMU.

## C++ `class` / `struct` declaration order

Plain data and aggregate `struct` declarations place necessary inner types, aliases and constants first, then data fields, followed by comparison operators and other small methods. Do not force class-style method-first ordering onto data carriers.

For behavior-oriented types, arrange declarations in this order:

1. Inner types and type aliases: `using`, `enum`, nested `struct` and nested `class`.
2. Static data members and class constants, including `static constexpr` values.
3. Static function declarations.
4. Non-inline member function declarations, including constructors, destructors and deleted operations.
5. Inline getters and accessors.
6. Other inline member functions.
7. Instance data members.

Within each section, place `public` declarations before `private` declarations. Start the next section with `public` again when needed; repeating access specifiers is expected. Omit empty blocks and `private:` at the start of a `class`.

Group functions by responsibility and implementation file with blank lines. Within every group, declarations in the header and definitions in the source file must use the same order.

## Functions, expressions and control flow

- Keep the normal path readable top to bottom; use early returns for invalid inputs, unchanged state and independent exits.
- Keep simple ternaries and short branches compact. Give an expression that requires repeated parsing a clear branch or a meaningfully named intermediate value; do not add variables that only rename an obvious expression.
- Keep an object's invariants within an explicit operation. Refresh derived state there when required, rather than making every caller remember a separate `refresh()`.
- Use local lambdas for small operations near their use. Extract from long callbacks only genuinely independent operations.
- A genuinely short single-statement `if`, `for` or `while` may remain on one line. If its body starts on the next line, enclose it in braces; nested multiline control flow needs braces at every level.
- In a `switch`, write multi-statement cases as `case X: {`, with any terminating `break;` inside the braces.

## Blank-line grouping

- Use one blank line between distinct responsibilities, phases or ideas; keep statements forming one thought or operation together.
- Separate class declaration categories, function groups and independent member-state groups with one blank line; keep closely related overloads and accessors together. Add short group-heading comments only when numerous variables or functions make the grouping hard to follow.
- In function bodies, group related declarations with the logic that consumes them. Separate setup, validation, state changes and externally visible effects only when they are genuinely distinct steps; those steps do not each need a separate function.
- Separate function definitions with one blank line. Do not put blank lines between every statement or closely related declaration, or use consecutive blank lines.
- Indented blank lines retain their scope's indentation.

## Variable grouping and layout constants

- Group member variables by responsibility, not scalar type: keep related data, counters, timestamps and flags together.
- Within a group, place dependencies before derived values and keep constructor initializers in declaration order.
- Declare locals near first use, alongside the operation that consumes them.
- Closely related scalars, whether fields, members or locals, may share a declaration, such as `int32_t x = 0, y = 0, width = 0, height = 0;`. Do not combine unrelated variables merely to save lines.
- Arrange UI declarations, initialization and drawing in reading or display order where practical; dependencies and lifetimes take priority.
- Give meaningful names to colors, durations and capacities. Derive related positions and dimensions from content, spacing and existing constants; do not merely rename independent magic numbers or extract every `0`, `1` or simple formula.

## Hand-written formatting

- Use tabs for indentation (width 4) and spaces for alignment. Align opening braces within adjacent groups of single-line functions, never across blank lines or constructor initializer lists.
- Do not impose a hard column limit or mechanically put each parameter on its own line. Short functions, conditions and small enums may stay on one line; do not compress complex code to make it fit.
- Use `{ value, size }` for aggregates (spaces inside the braces), `T*` / `T&` / `const T&` for pointers and references (markers attached to the type), and `[] (...)` for lambdas with parameters.
- Keep short constructor initializer lists compact; organize longer lists by member and aggregate contents by element and logical group. A long list starts on its own line indented one level with `: _a(a), _b(b)`, and the body brace follows on the next line.
- Keep template parameters, constraints and function declarations visually distinct; do not crowd multiline `requires` expressions into function bodies or disrupt parameter layout.
- Indent multiline lambda bodies and conditional-compilation directives with their code scope.

## Performance and compile-time checks

- Keep hot paths free of added allocations, copies, locks, formatting and traversals, and preserve their inlining, locality and existing optimized paths when moving code.
- Preserve the direct path when optional drawing features are unused; using some features must not trigger expensive processing for others.
- Check statically known capabilities, capacities and index mappings with concepts and `static_assert`; unsupported operations must not be silently ignored.
- Use `constexpr` for computations suitable for constant evaluation and `consteval` where the result must be computed at compile time, not as decoration on runtime drawing or I/O functions.
- Force inline short hot operations or forwarding that must disappear, not as a blanket annotation.
- Use `[[nodiscard]]` when discarding the result is likely a mistake; omit it when ignoring the result is a valid use.
- Choose dispatch according to its use and cost: hot rendering and infrequent UI callbacks have different needs. Do not force every callback into templates or ban appropriate existing `std::function` use.
- Measure or inspect generated code when a change has a concrete performance risk. Do not create temporary test projects for ordinary formatting changes.

## Tests

- Tests use no framework. Each `*Test.cpp` defines `static void` functions inside the project namespace, named as a sentence such as `messagesHaveStableWireEncoding`, checks with the `CHECK` macro from `Test.hpp` and calls them from `main()`.
