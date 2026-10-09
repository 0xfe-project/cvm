/*
 * cvm/try.h -- the `try` shape generated code emits.
 *
 *     CVM_TRY() {
 *         ... body ...
 *     } CVM_CATCH(exc) {
 *         ... handler ...
 *     } CVM_ENDTRY();
 *
 * Three things about this are deliberate.
 *
 * 1. It expands to `if`/`else`, so `break` and `return` out of a body behave
 *    the way they read. A macro built out of `for` and `do` -- the usual way
 *    to do this -- makes `break` mean "leave the try", which is not what any
 *    frontend wants to have to remember.
 *
 * 2. The handler is looked up from the thread whenever a catch needs it,
 *    rather than being held in a local. A local assigned between `setjmp`
 *    and `longjmp` has an indeterminate value after the jump -- that is the
 *    rule the C standard states and the reason generated code has to spill
 *    anything it needs across a try. A name the runtime owns has no such
 *    problem, and it is also what lets this header keep no state of its own.
 *
 * 3. The pop happens before the catch body runs, so a `throw` inside a catch
 *    finds the enclosing handler rather than re-entering the same one.
 *
 * This header includes rt/rt_jump.h, and so does any translation unit that
 * uses it: `setjmp` has to run in the function that will be jumped to, so it
 * cannot be wrapped in a function. See that header.
 *
 * A `finally` is not here yet. It is a frontend contract question -- what it
 * means for a body that both returns and throws -- and it belongs with the
 * rest of that contract rather than being guessed at here.
 */
#if !defined(CVM_TRY_H)
#define CVM_TRY_H

#include "cvm/exc.h"
#include "rt_jump.h"

#define CVM_TRY() if (rt_jump_set(cvm_handler_push()) == 0) {
/*
 * The handler stays linked for the whole catch, so the value the body works
 * with is rooted by the chain rather than by a local holding it. A local
 * would be indeterminate across the jump in any case -- that is the rule
 * which makes generated code spill whatever it needs across a try -- so
 * there is nothing to be gained by handing one over.
 */
#define CVM_CATCH(exception)                                                           \
    cvm_handler_pop();                                                                 \
    }                                                                                  \
    else {                                                                             \
        opaque_mut exception = cvm_handler_begin_catch();

#define CVM_ENDTRY()                                                                   \
    cvm_handler_pop();                                                                 \
    }

#endif /* CVM_TRY_H */
