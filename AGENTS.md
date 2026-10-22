# AGENTS.md

## Authority

- The Chinese version of AGENTS is located at `docs/AGENTS_CN.md`. Do not modify that file.
- Do not modify any other files in the `docs` directory.
- Place generated documentation in `docs-codex`.
- Merge and update the project's latest implemented technical functionality in `docs-codex/DESIGN.md`.
- Update `docs-codex/HISTORY.md` with a brief report of the current operation. The source continues with the incomplete phrase "more than 10 operations ago and the operation has already been completed"; the intended action for those entries is unspecified.

## Design Requirements

- Focus only on the functionality being modified.
- Before each modification, stage the files to be modified in Git.
- Do not modify readme.md unless explicitly instructed to do so.
- If a file already exists, first analyze and follow the instructions it contains.
- If a function's author attribution includes authors other than codex, respect the original approach and follow the function's existing style when modifying it.
- Preserve the existing code structure and keep diffs as small as possible.

## Comment Requirements

- Add comments only if they do not affect code execution.

## Commit Requirements

- Before committing, check whether `.gitignore` needs additional entries. Do not add intermediate files or build outputs to Git, and do not expose information about the development computer.
- Preserve existing copyright, license, and other valid attribution notices when committing.
- Use `codex` as the commit author.

## Testing Requirements

- Place the test report for the current version in `docs-codex/test-report.md`, and remove testing content related to the previous version.

## C Coding Standards

See [.clang-format](.clang-format) for automatic formatting rules. This document lists only supplementary conventions that must be followed manually.

### Files and Organization

- Use UTF-8 for source code. Use lowercase file and directory names, preferably separating words with underscores. Names should reflect their purpose.
- Header files must have include guards using `__<FILE>_H_`, as specified in the original standard. Include only declarations, type definitions, and similar content; do not include function bodies.
- Organize header files in this order: file comments, preprocessor section, then type and function declarations. Organize source files in this order: file comments, header includes, then data and function implementations.
- Order header includes as follows: the corresponding header, C standard library headers, other library headers, then internal project headers. Follow project conventions for ordering within each category.

### Additional Manual Formatting Requirements

- Separate relatively independent functions and blocks such as `if/switch/for/do/while` from other code with blank lines before and after them.
- Place standalone explanatory comments immediately above the corresponding code, separated from preceding unrelated code by a blank line. Do not leave a blank line between a comment and the code it describes. Comments for individual statements or structure fields may also appear to their right, but not below them.
- `#include` and `#define` directives are exempt from column limits. When manually formatted macros must be preserved, use `clang-format off/on` only for the necessary region. Check excessively long identifiers and similar cases manually.

### Naming

- Use PascalCase for types, such as `SensorData`. Use lowercase words separated by underscores for variables, parameters, and structure members, such as `sample_count`.
- Use `k` followed by PascalCase for constants, such as `kDaysInAWeek`. Use uppercase words separated by underscores for macros and enumeration values.
- Use mixed-case names for ordinary functions. The original text does not specify the case of the first letter; this document standardizes on PascalCase, such as `ReadSensor`. Getter and setter names should match the variable name, such as `get_count` and `set_count`.

### Comments

- Meaningful comment lines must account for at least 30% of the total code line count. Blank lines, empty comments, and unfilled template lines do not count as meaningful comments. Chinese comments are recommended.
- Use `//` for single-line comments.
- Add comments to generated code according to its functional blocks.
- Comments are required for data structures that are not sufficiently self-explanatory; variables, constants, and macros with physical meaning; and complex, critical, or non-obvious implementations. For physical quantities, explain their meaning and units.
- When modifying code, explain how and why it was changed. Keep only the latest modification explanation at a given location. Use `TODO` for temporary or incomplete code.
- At the beginning of each file, state the filename, responsible person, creation date, purpose, revision history, copyright, and legal information. Leave fields blank where no relevant information applies.

  ```c
  /**
   * @file Filename
   * @author [Original author],Codex
   * @brief Brief description of functionality
   * @date Creation date
   * @copyright Copyright (c) Year
   */
  ```

- Use the following template consistently for function comments. Include necessary input/output details, null-pointer constraints, memory ownership, performance requirements, and synchronization prerequisites in the relevant descriptions. Add necessary implementation details at the function definition.

  ```c
  /**
   * @authors [Original author],Codex
   * @brief Brief description of functionality
   * @details (Optional) Detailed description
   * @param parameter_name Parameter description. Use one `@param parameter_name description` entry per parameter; omit `@param` if there are no parameters.
   * @return Description of the return value. Use `@return None` when there is no return value and the return type is `void`.
   * @note (Optional) Additional notes
   */
  ```

### Macros, Variables, and Expressions

- Fully parenthesize each parameter and the entire expression in expression macros. Enclose multi-statement macros in braces; `do { ... } while (0)` is recommended.
- Initialize basic types and pointers when defining them. Initialize arrays, structures, and unions at their definition or immediately afterward.
- Local variables must not have the same names as global variables. Remove unused global variables and prefer local variables. Use `static` for module-private global variables, and use `extern` only when necessary.
- Except for simple assignments, use parentheses to make the evaluation order of compound expressions explicit. Do not use numbers whose meaning is unclear. Use meaningful macros or enumerations for physical states and quantities; when numbers must be used directly, explain their meaning.

### Control Statements and Functions

- Do not compare floating-point values directly with numbers using `==` or `!=`. Use tolerance or range comparisons based on the precision required by the application. Use `NULL` for pointer null checks. Placing constants first in conditional comparisons is recommended.
- Do not modify the loop variable inside a `for` loop body. A `switch` must have a `default` branch that performs actual handling; if it must be omitted, explain why.
- Specify both the type and name of function parameters. Using `void` for functions with no parameters is recommended. Place input parameters before output parameters, and pass structure parameters by pointer.
- Keep functions short and their responsibilities clear. Reentrant functions that use shared state or `static` local variables must handle synchronization.
- Interrupt handlers must not call time-consuming or blocking functions, or functions that do not meet interrupt-context requirements, such as printing, blocking waits, or dynamic memory allocation.

### Memory and Maintenance

- Check for `NULL` immediately after allocating dynamic memory. Initialize successfully allocated memory before reading it. Ensure memory is released correctly on every execution path.
- Set the corresponding pointer to `NULL` immediately after `free`, and prevent other references from continuing to access the freed memory.
- A consistent debug output format is recommended, including the module name or filename and line number. Address compiler warnings. Preserve the local style when modifying existing code; existing noncompliant code may be cleaned up gradually according to project conventions.
