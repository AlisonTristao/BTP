#ifndef BTP_PROFILES_LOOPCTL_H
#define BTP_PROFILES_LOOPCTL_H

/*
 * loopctl -- an OPTIONAL convention for writing a controller once and running
 * it unchanged on a host and on an embedded target.
 *
 * It belongs to the `loop` profile (docs/profiles/loop.md). Like the profile it
 * is NOT part of the BTP protocol, nothing in the BTP library depends on it,
 * and an endpoint is free to ignore it. It does not mention BTP at all: a
 * controller implementing it is a plain C object, usable with or without a
 * transport.
 *
 * Contract
 * --------
 *  - Data layout. in->y holds ny measurements; out->u holds `horizon` steps of
 *    nu inputs, step-major: the value of input i at step k is u[k*nu + i].
 *    Step k is the value intended for plant tick (in->tick + 1 + k).
 *  - Order. y[] follows the declaration order of the application's `loop.state`
 *    measurement fields; u[] follows the declaration order of the application's
 *    `loop.u` inputs.
 *  - out->horizon is a capacity on input (steps the caller can hold) and the
 *    number of steps actually written on output (at least 1 and at most the
 *    capacity when step returns 0). out->u must have room for
 *    nu * capacity floats.
 *  - No allocation inside step(). Any memory step() needs is obtained in init()
 *    and released in destroy(). step() should have bounded execution time.
 *  - Return values: 0 means success; any other value is an error and the
 *    contents of out are then unspecified.
 *  - The same source compiles on a host and on an embedded target: C89/C99, no
 *    operating-system or library dependency beyond <stdint.h>.
 *
 * Lifecycle: init -> (step | set_param | reset)* -> destroy. `state` is the
 * controller's private data; init() sets *state, destroy() releases it.
 * init() receives the initial parameters (name, value, unit and range as
 * declared by the application); set_param() changes one at run time and
 * returns non-zero for an unknown name or a value outside its range.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One call of step(): the latest measurements. */
typedef struct {
  uint32_t tick;     /* plant tick the measurements belong to (loop.state.tick) */
  float dt;          /* nominal period of one tick, in seconds */
  const float* y;    /* ny measurements, loop.state declaration order */
  uint16_t ny;
} loopctl_in;

/* The plan produced by step(). */
typedef struct {
  float* u;          /* horizon * nu floats, u[k*nu + i] */
  uint16_t nu;       /* inputs per step, loop.u declaration order */
  uint16_t horizon;  /* capacity in, steps written out */
} loopctl_out;

/* A tunable parameter: name, current value, unit and range. */
typedef struct {
  const char* name;
  float value;
  const char* unit;
  float min;
  float max;
} loopctl_param;

typedef struct {
  const char* name;
  int  (*init)(void** state, const loopctl_param* params, uint16_t nparams);
  int  (*step)(void* state, const loopctl_in* in, loopctl_out* out); /* 0 = ok */
  int  (*set_param)(void* state, const char* name, float value);
  void (*reset)(void* state);
  void (*destroy)(void* state);
} loopctl_controller;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* BTP_PROFILES_LOOPCTL_H */
