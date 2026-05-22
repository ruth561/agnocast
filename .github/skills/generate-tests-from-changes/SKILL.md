---
name: Generate Tests from Code Changes
description: Analyzes code changes (diffs, new features, refactoring) and generates comprehensive test cases following white-box testing patterns. Use when: implementing new functions, refactoring code, or need thorough test coverage for changes.
---

# Generate Tests from Code Changes

## Overview

This skill automates the generation of comprehensive white-box unit tests based on code changes. It follows the established testing patterns from the agnocast codebase and generates tests that cover happy paths, edge cases, and error handling scenarios.

## Test Structure Pattern

Tests in this codebase follow the **AAA (Arrange-Act-Assert) pattern** and are organized by function with clear naming conventions. All tests inherit from a test fixture base class that handles common setup, teardown, and utility methods.

### Core Structure Elements

```cpp
class TestFunction : public ::testing::Test
{
protected:
  void TearDown() override { /* cleanup */ }
  
  // Helper methods for test construction
  std::shared_ptr<MyClass> make_object_at(int value) const { /* ... */ }
};

TEST_F(TestFunction, descriptor_of_what_is_being_tested)
{
  // Arrange — set up preconditions and test data
  
  // Act — perform the operation being tested
  
  // Assert — verify the results
}
```

### Naming Convention

**Test names** follow the format: `function_name_behavior_description`

Examples:
- `handle_pre_time_jump_saves_remaining_period_as_time_credit`
- `handle_timer_event_catches_up_many_periods_at_once`
- `set_period_updates_period_only_without_modifying_call_time_anchors`

**Test classes** use the function or subsystem name: `TestFunctionName` or `TestSubsystem`

### Comment Format

Use strategic comments to explain complex test logic:

```cpp
// Arrange — set up the test scenario with deterministic values
const int64_t now_ns = 500'000'000;  // 0.5s on the new epoch
auto clock = make_ros_clock_at(now_ns);
auto info = make_timer_info(clock, now_ns);
// Pin next_call so the credit is deterministic: now + 30ms.
const int64_t next_call_ns = now_ns + 30'000'000;
info->next_call_time_ns.store(next_call_ns, std::memory_order_relaxed);

// Act — invoke the function under test
agnocast::handle_pre_time_jump(*info);

// Assert — verify expected outcome with clear expectations
EXPECT_EQ(info->time_credit.load(std::memory_order_relaxed), next_call_ns - now_ns);
```

### Test Depth Per Function

For each function, generate tests at three levels:

1. **Happy Path** — function executes normally with valid inputs
2. **Edge Cases** — boundary conditions, zero values, extreme ranges
3. **Error Handling** — invalid inputs, exceptions, resource failures

**Minimum test count per function:** 3 tests (1 happy path + 2 edge/error cases)
**Target test count per function:** 5-7 tests for comprehensive coverage

## Workflow: 5-Step Test Generation Process

### Step 1: Analyze Code Changes

**Input:** Git diff or code changes
**Output:** Change summary document

Examine the modified code and identify:
- **New functions** introduced
- **Modified logic** paths (conditions, loops, state mutations)
- **Public API changes** vs. internal refactoring
- **Dependencies** and assumptions
- **Resource handling** (memory, file handles, synchronization)

**Example Analysis:**
```
Function: handle_post_time_jump
Changed: Added RCL_ROS_TIME_ACTIVATED case with timer_fd close logic
Paths: 3 branches (ROS_TIME_ACTIVATED, ROS_TIME_DEACTIVATED, forward jump)
Resources: Opens/closes eventfd, manages weak_ptr timers
Dependencies: RCL clock interface, atomic stores
```

### Step 2: Generate Test Scenarios

**Input:** Change analysis from Step 1
**Output:** List of test scenarios with descriptions

For each function path, create scenarios covering:
- **Normal operation** (valid preconditions, expected behavior)
- **Boundary/edge cases** (off-by-one, zero values, limits)
- **Error conditions** (invalid state, exceptions, timeouts)
- **Resource states** (uninitialized, null/expired pointers)
- **Concurrency** (atomic operations, synchronization)

**Example Scenarios for `handle_post_time_jump`:**

| Scenario | Description | Expected Outcome |
|----------|-------------|------------------|
| ROS_TIME_ACTIVATED, ready timer | Timer past due after ROS time activated | Closes timer_fd, applies time_credit to anchors |
| ROS_TIME_ACTIVATED, no credit | Timer activated but no credit saved | Closes timer_fd, leaves anchors unchanged |
| ROS_TIME_ACTIVATED, uninitialized clock | Clock at t=0 during activation | Closes timer_fd, preserves credit, skips anchor update |
| Forward jump, timer ready | Clock jumped ahead past next_call_time | Writes to clock_eventfd, invokes callback |
| Forward jump, canceled timer | Timer ready but already canceled | Does NOT write to clock_eventfd |
| Forward jump, not ready | Clock jumped but timer not yet due | No event written, anchors untouched |
| Backward jump, past last_call | Clock jumped backward before last_call_time | Resets anchors to now |
| Backward jump, within period | Clock jumped backward within one period | Leaves anchors unchanged |

### Step 3: Generate Test Code

**Input:** Test scenarios from Step 2
**Output:** Complete test methods with AAA structure

For each scenario, generate:

```cpp
TEST_F(TestFunctionName, scenario_name_descriptor)
{
  // Arrange — setup with explicit values and comments
  // • Set preconditions needed for this path
  // • Use helper methods for common constructs (make_*, setup_*)
  // • Include comments on why values are chosen (sentinel, determinism)
  
  // Act — single function call being tested
  
  // Assert — verify all relevant state changes
  // • Check primary output/return value
  // • Verify side effects (state mutations, writes)
  // • Use EXPECT_* for non-fatal checks when appropriate
  // • Combine related assertions with detailed error messages
}
```

**Key Patterns:**

1. **Snapshot pattern** — Save state before Act, compare after:
   ```cpp
   // Arrange
   const int64_t snapshot_last = info->last_call_time_ns.load(...);
   
   // Act
   agnocast::handle_post_time_jump(*info, jump);
   
   // Assert
   EXPECT_EQ(info->last_call_time_ns.load(...), snapshot_last);
   ```

2. **Sentinel pattern** — Use distinct values to detect unwanted mutations:
   ```cpp
   // Arrange — use sentinel that differs from would-be computed value
   const int64_t would_be_credit = info->next_call_time_ns.load(...);
   const int64_t sentinel = would_be_credit + 1;  // Obviously different
   info->time_credit.store(sentinel, std::memory_order_relaxed);
   
   // Assert — verify sentinel unchanged (function was no-op)
   EXPECT_EQ(info->time_credit.load(...), sentinel);
   ```

3. **Exception safety pattern** — Verify no exceptions escape:
   ```cpp
   // Act / Assert — exception must not escape
   EXPECT_NO_THROW(agnocast::handle_pre_time_jump(*info));
   EXPECT_EQ(info->time_credit.load(...), sentinel);
   ```

4. **Resource tracking pattern** — Verify resource lifecycle:
   ```cpp
   // Arrange — open a real resource as stand-in
   info->timer_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
   ASSERT_GE(info->timer_fd, 0);
   
   // Act
   agnocast::handle_post_time_jump(*info, jump);
   
   // Assert — verify resource was properly closed
   EXPECT_EQ(info->timer_fd, -1);
   ```

### Step 4: Quality Check

**Input:** Generated test code from Step 3
**Output:** Quality assessment and refinement feedback

Validate each test against:
- ✓ AAA structure correctly applied (Arrange, Act, Assert clearly separated)
- ✓ Single responsibility (one logical behavior per test)
- ✓ Clear naming (test name describes what is being tested)
- ✓ Deterministic setup (no flaky tests, explicit values, seeds)
- ✓ Assertion coverage (all relevant state mutations verified)
- ✓ Comment clarity (non-obvious preconditions explained)
- ✓ Resource cleanup (no leaks, proper teardown)
- ✓ Helper method usage (DRY principle, consistent patterns)

**Refinements:**
- Combine redundant assertions into single EXPECT_ calls with messages
- Add explanatory comments for sentinel values or magic numbers
- Move duplicate setup into helper methods
- Verify correct use of atomic load/store with proper memory ordering
- Check that exceptions are handled appropriately

### Step 5: Finalize and Document

**Input:** Quality-checked tests from Step 4
**Output:** Final test file with documentation

Deliverables:
- Complete test file (.cpp) ready to compile
- Test fixture class with necessary helpers
- All tests implemented in AAA format
- Comments explaining non-obvious decisions
- Cleanup (TearDown) that prevents test pollution

## Concrete Examples

### Example 1: Happy Path Test

**Function:** `set_period(int64_t new_period_ns)`
**Scenario:** Update period without modifying call-time anchors

```cpp
TEST_F(TestTimer, set_period_updates_period_only_without_modifying_call_time_anchors)
{
  // Arrange — last_call != now catches an anchor-to-now regression
  // (set_period must not behave like reset).
  const int64_t last_call_ns = 1'000'000'000;
  const int64_t now_ns = 1'200'000'000;
  auto clock = make_ros_clock_at(now_ns);
  auto info = make_timer_info(clock, /*now_ns=*/last_call_ns);
  const int64_t old_period = info->period_ns.load(std::memory_order_relaxed);
  const int64_t new_period = 50'000'000;  // Change to 50ms
  
  // Act
  info->set_period(std::chrono::nanoseconds{new_period});
  
  // Assert
  EXPECT_EQ(info->period_ns.load(std::memory_order_relaxed), new_period);
  EXPECT_EQ(info->last_call_time_ns.load(std::memory_order_relaxed), last_call_ns);
  EXPECT_EQ(info->next_call_time_ns.load(std::memory_order_relaxed), last_call_ns + old_period);
}
```

### Example 2: Edge Case Test

**Function:** `handle_timer_event()`
**Scenario:** Period=0 timer fires exactly once, not infinitely

```cpp
TEST_F(TestTimer, handle_timer_event_with_zero_period_sets_next_call_to_now)
{
  // Arrange — period=0 timer with stored next_call far in the past.
  // Callback must fire exactly once (no infinite-loop in the catch-up branch).
  const int64_t stored_next = 100;
  const int64_t now_ns = 1'000'000'000;
  auto clock = make_ros_clock_at(now_ns);
  auto info = make_timer_info(clock, stored_next, /*period_ns=*/0);
  int call_count = 0;
  std::function<void()> cb = [&call_count]() { ++call_count; };
  auto timer = std::make_shared<agnocast::GenericTimer<std::function<void()>>>(
    0u, std::chrono::nanoseconds{0}, clock, std::move(cb));
  info->timer = timer;
  
  // Act
  agnocast::handle_timer_event(*info);
  
  // Assert
  EXPECT_EQ(info->next_call_time_ns.load(std::memory_order_relaxed), now_ns);
  EXPECT_EQ(call_count, 1) << "period=0 must fire exactly once per call, not loop";
}
```

### Example 3: Error Handling Test

**Function:** `handle_pre_time_jump()`
**Scenario:** Exception from clock->now() is swallowed gracefully

```cpp
TEST_F(TestTimer, handle_pre_time_jump_swallows_exception_from_clock_now)
{
  // Arrange — RCL_CLOCK_UNINITIALIZED has no get_now hook, so clock->now()
  // throws rclcpp::exceptions::RCLError (a std::exception).
  auto clock = std::make_shared<rclcpp::Clock>(RCL_CLOCK_UNINITIALIZED);
  auto info = make_timer_info(clock, /*now_ns=*/0);
  const int64_t would_be_credit = info->next_call_time_ns.load(std::memory_order_relaxed);
  const int64_t sentinel = would_be_credit + 1;
  info->time_credit.store(sentinel, std::memory_order_relaxed);
  
  // Act / Assert — exception must not escape
  EXPECT_NO_THROW(agnocast::handle_pre_time_jump(*info));
  EXPECT_EQ(info->time_credit.load(std::memory_order_relaxed), sentinel);
}
```

## Usage Instructions

### When to Use This Skill

✓ **After implementing new functions** — Generate comprehensive tests immediately
✓ **During refactoring** — Ensure behavior is preserved with targeted tests
✓ **For bug fixes** — Create regression tests following the patterns
✓ **Code review** — Suggest test cases for submitted changes
✓ **Before merging** — Verify test coverage for changed code

### How to Invoke

Provide one of the following to the skill:

1. **A git diff:**
   ```
   Please generate white-box unit tests for these changes:
   
   ```diff
   @@ -50,10 +50,25 @@ void handle_timer_event(TimerInfo& info)
   ...
   ```
   ```

2. **A code snippet with context:**
   ```
   I added this function to handle time jumps. Please generate tests:
   
   void handle_post_time_jump(TimerInfo& info, const rcl_time_jump_t& jump) {
     // implementation...
   }
   ```

3. **A reference to an existing file:**
   ```
   Generate comprehensive tests for the changes in src/timer.cpp (lines 50-150).
   ```

### Output Format

The skill delivers:

1. **Test Fixture Class** — Inherits from `::testing::Test`, includes helpers
2. **Test Methods** — Organized by function, following AAA pattern
3. **Helper Methods** — Reusable setup and validation utilities
4. **Documentation Comments** — Explaining non-obvious test design decisions
5. **Ready-to-compile Code** — Includes necessary headers and using statements

**File naming:** `test_<module_name>.cpp` (e.g., `test_agnocast_timer.cpp`)

### Integration with Existing Tests

- Add new tests to the existing test file for the module
- Use existing helper methods from the fixture class
- Follow the same atomic operation patterns and error handling
- Maintain consistent test grouping with section comments

## Quality Checklist

Use this checklist to verify generated tests meet the quality standard:

### Structure (AAA Pattern)
- [ ] Arrange section clearly separated from Act and Assert
- [ ] Each test has exactly one Act section (single function call)
- [ ] Assert section directly follows Act
- [ ] Comments mark Arrange/Act/Assert boundaries

### Naming & Clarity
- [ ] Test name: `function_behavior_description` format
- [ ] Name accurately describes what is being tested
- [ ] Class name: `Test<FunctionName>` or `Test<Subsystem>`
- [ ] Comments explain why values are chosen (not just what they are)

### Correctness
- [ ] Test is deterministic (no flaky timing/randomness)
- [ ] Test preconditions are explicitly set
- [ ] All modified state is verified in assertions
- [ ] Related assertions are combined with clear error messages

### Coverage
- [ ] Happy path: normal operation with valid inputs
- [ ] Edge cases: boundaries, zero values, limits
- [ ] Error handling: invalid state, exceptions
- [ ] Resource lifecycle: initialization, cleanup, lifecycle transitions

### Resource Management
- [ ] No resource leaks in test code
- [ ] TearDown() properly cleans up test state
- [ ] Atomic operations use consistent memory ordering
- [ ] File descriptors/handles properly closed

### Reusability
- [ ] Common setup factored into helper methods
- [ ] Helper methods follow `make_*` or `setup_*` naming
- [ ] Avoid duplication across tests
- [ ] Helper methods have clear, documented behavior

### Documentation
- [ ] Non-obvious sentinel values explained
- [ ] Preconditions and why they matter are documented
- [ ] Boundary conditions noted in comments
- [ ] Expected failures or edge cases explained

## Key Patterns Reference

### Pattern 1: Testing State Mutation

Snapshot state before the operation, verify change after:

```cpp
const int64_t before = object->field.load(std::memory_order_relaxed);
operation(object);
EXPECT_EQ(object->field.load(std::memory_order_relaxed), expected_after);
```

### Pattern 2: Testing No-Op Cases

Use sentinel values (clearly different from computed values):

```cpp
const int64_t sentinel = computed_value + 1;  // Obviously wrong if stored
object->field.store(sentinel, std::memory_order_relaxed);
operation(object);  // Should not modify field
EXPECT_EQ(object->field.load(std::memory_order_relaxed), sentinel);
```

### Pattern 3: Testing Exception Safety

Wrap operation and verify state is preserved:

```cpp
const int64_t snapshot = object->field.load(std::memory_order_relaxed);
EXPECT_NO_THROW(dangerous_operation(object));
EXPECT_EQ(object->field.load(std::memory_order_relaxed), snapshot);
```

### Pattern 4: Testing Resource Lifecycle

Open real resources or mocks, verify proper cleanup:

```cpp
// Arrange
object->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
ASSERT_GE(object->fd, 0);

// Act
operation_that_closes_fd(object);

// Assert
EXPECT_EQ(object->fd, -1);  // Verify closed
```

### Pattern 5: Testing Callback Invocation

Use function wrapper or counter to track invocations:

```cpp
int call_count = 0;
std::function<void()> cb = [&call_count]() { ++call_count; };
auto obj = std::make_shared<MyObject>(std::move(cb));
operation(obj);
EXPECT_EQ(call_count, 1);
```

## Common Pitfalls to Avoid

- ❌ Multiple function calls in Act section → **Single call only**
- ❌ Testing implementation details instead of behavior → **Test observable effects**
- ❌ Missing comments on magic numbers → **Always explain precondition values**
- ❌ Incomplete assertion of side effects → **Verify all state changes**
- ❌ Flaky tests with timing-dependent logic → **Use explicit values, not sleeps**
- ❌ Test pollution (test A affects test B) → **TearDown() must clean up**
- ❌ Weak assertions (e.g., just checking not-null) → **Verify actual values/behavior**

## Performance Considerations

- Keep tests focused and fast (unit tests should complete in milliseconds)
- Avoid creating expensive resources in every test (use helper builders)
- Use mocks/fakes for expensive dependencies (file I/O, network, etc.)
- Pre-compute expected values rather than computing them dynamically in assertions

## References

- **Reference Implementation:** [test_agnocast_timer.cpp](../../src/agnocastlib/test/unit/test_agnocast_timer.cpp)
- **Testing Framework:** Google Test (gtest) C++17
- **Style Guide:** Follow existing test file conventions in the agnocast codebase
