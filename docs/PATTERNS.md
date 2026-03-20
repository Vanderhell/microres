# Patterns

Real-world composition patterns showing how to combine microres primitives
with each other and with other libraries.

---

## 1. MQTT publish with exponential backoff

The most common pattern: retry a network call with increasing delays.

```c
static const mres_retry_policy_t mqtt_retry = {
    .max_attempts  = 5,
    .base_delay_ms = 500,
    .max_delay_ms  = 15000,
    .strategy      = MRES_BACKOFF_EXPONENTIAL,
    .jitter        = true,    /* decorrelate multiple devices */
};

static int do_publish(void *ctx) {
    mqtt_msg_t *msg = (mqtt_msg_t *)ctx;
    return mqtt_publish(msg->topic, msg->payload, msg->qos);
}

void publish_telemetry(mqtt_msg_t *msg) {
    mres_retry_t retry;
    mres_retry_init(&retry, &mqtt_retry);

    int result = mres_retry_exec(&retry, do_publish, msg, hal_clock, hal_delay);
    if (result != MRES_OK) {
        log_error("Publish failed after %d attempts: %s",
                  retry.attempts, mres_err_str(result));
        /* Optionally: spool to iotspool for later delivery */
    }
}
```

---

## 2. Circuit breaker protecting a broker

When the broker is down, stop hammering it. Probe periodically.

```c
static const mres_breaker_policy_t broker_breaker_policy = {
    .failure_threshold    = 3,
    .recovery_timeout_ms  = 60000,   /* 1 minute cooldown */
    .half_open_max_calls  = 1,
};

static mres_breaker_t broker_breaker;

void system_init(void) {
    mres_breaker_init(&broker_breaker, &broker_breaker_policy);
}

int resilient_publish(mqtt_msg_t *msg) {
    int result = mres_breaker_call(&broker_breaker, do_publish, msg, hal_clock);

    if (result == MRES_ERR_OPEN) {
        /* Breaker is open — broker is down */
        uint32_t wait = mres_breaker_remaining_ms(&broker_breaker, hal_clock);
        log_warn("Broker down, retry in %lu ms", wait);
        spool_enqueue(msg);  /* save for later */
        return result;
    }

    return result;
}
```

---

## 3. Retry inside a circuit breaker

Combine both: retry a few times on each attempt, but if the resource is
consistently failing, open the breaker.

```c
static const mres_retry_policy_t quick_retry = {
    .max_attempts  = 3,
    .base_delay_ms = 200,
    .max_delay_ms  = 1000,
    .strategy      = MRES_BACKOFF_EXPONENTIAL,
    .jitter        = false,
};

static int publish_with_retry(void *ctx) {
    mres_retry_t retry;
    mres_retry_init(&retry, &quick_retry);
    return mres_retry_exec(&retry, do_publish, ctx, hal_clock, hal_delay);
}

int robust_publish(mqtt_msg_t *msg) {
    /* The breaker wraps the retry — if 3 retried attempts fail,
       the breaker counts that as one failure. After 3 such failures
       (= 9 total attempts), the breaker opens. */
    return mres_breaker_call(&broker_breaker, publish_with_retry, msg, hal_clock);
}
```

---

## 4. Rate-limited API calls

Throttle outbound API calls to respect rate limits.

```c
static const mres_ratelimit_policy_t api_limit = {
    .max_tokens   = 10,      /* 10 requests */
    .refill_ms    = 60000,   /* per minute */
    .refill_count = 10,
};

static mres_ratelimit_t api_limiter;

void system_init(void) {
    mres_ratelimit_init(&api_limiter, &api_limit, hal_clock);
}

int send_to_cloud(telemetry_t *data) {
    if (!mres_ratelimit_acquire(&api_limiter, 1, hal_clock)) {
        log_info("Rate limited, queueing");
        return MRES_ERR_RATE_LIMITED;
    }

    return http_post("/api/telemetry", data);
}
```

---

## 5. Rate limit → breaker → retry (full stack)

The complete resilience pipeline for a critical operation:

```c
typedef struct {
    mres_ratelimit_t  limiter;
    mres_breaker_t    breaker;
    void             *payload;
} resilient_ctx_t;

static int inner_operation(void *ctx) {
    resilient_ctx_t *r = (resilient_ctx_t *)ctx;
    return http_post("/api/data", r->payload);
}

int resilient_send(resilient_ctx_t *ctx) {
    /* 1. Rate limit */
    if (!mres_ratelimit_acquire(&ctx->limiter, 1, hal_clock)) {
        return MRES_ERR_RATE_LIMITED;
    }

    /* 2. Circuit breaker wraps retry */
    mres_retry_t retry;
    mres_retry_init(&retry, &quick_retry);

    /* Wrap retry in a lambda-like static function */
    int result = mres_breaker_call(&ctx->breaker, retry_inner, ctx, hal_clock);

    return result;
}
```

---

## 6. Integration with microfsm

Use the circuit breaker state as a guard condition in a state machine.

```c
#include "mfsm.h"
#include "mres.h"

static mres_breaker_t broker_breaker;

/* Guard: only allow transition to PUBLISHING if breaker is closed */
static bool guard_broker_available(void *ctx) {
    return mres_breaker_state(&broker_breaker) != MRES_BREAKER_OPEN;
}

static const mfsm_transition_t transitions[] = {
    { ST_ONLINE, EV_PUBLISH, ST_PUBLISHING, guard_broker_available, NULL },
    { ST_ONLINE, EV_PUBLISH, ST_QUEUING,    NULL,                   NULL },
    /* If broker unavailable, queue instead of publishing */
};
```

---

## 7. Integration with iotspool

Retry failed publishes from the spool with backoff.

```c
#include "mres.h"
/* #include "iotspool.h" */

static const mres_retry_policy_t spool_retry = {
    .max_attempts  = 3,
    .base_delay_ms = 2000,
    .max_delay_ms  = 30000,
    .strategy      = MRES_BACKOFF_EXPONENTIAL,
    .jitter        = true,
};

static int publish_from_spool(void *ctx) {
    spool_msg_t *msg = spool_peek();
    if (msg == NULL) return 0;  /* nothing to send */

    int err = mqtt_publish(msg->topic, msg->data, msg->len);
    if (err == 0) {
        spool_pop();  /* success — remove from spool */
    }
    return err;
}

void drain_spool(void) {
    while (spool_count() > 0) {
        mres_retry_t retry;
        mres_retry_init(&retry, &spool_retry);

        int result = mres_breaker_call(&broker_breaker,
                                        publish_from_spool_retry, NULL,
                                        hal_clock);
        if (result == MRES_ERR_OPEN) {
            log_info("Broker down, will retry spool later");
            return;  /* stop draining, breaker is open */
        }
    }
}
```

---

## 8. Sensor reading with retry

Retry an I2C sensor read that sometimes returns garbage.

```c
typedef struct {
    uint8_t  addr;
    float    value;
    uint8_t  valid_readings;
} sensor_ctx_t;

static int read_sensor(void *ctx) {
    sensor_ctx_t *s = (sensor_ctx_t *)ctx;
    float raw = i2c_read_float(s->addr);

    /* Sanity check */
    if (raw < -40.0f || raw > 125.0f) {
        return -1;  /* garbage reading */
    }

    s->value = raw;
    return 0;
}

static const mres_retry_policy_t sensor_retry = {
    .max_attempts  = 3,
    .base_delay_ms = 50,      /* short delay for I2C */
    .strategy      = MRES_BACKOFF_FIXED,
    .jitter        = false,
};

float get_temperature(uint8_t addr) {
    sensor_ctx_t ctx = { .addr = addr };
    mres_retry_t retry;
    mres_retry_init(&retry, &sensor_retry);

    if (mres_retry_exec(&retry, read_sensor, &ctx, hal_clock, hal_delay) == MRES_OK) {
        return ctx.value;
    }

    return NAN;  /* sensor failure */
}
```

---

## Pattern summary

| Pattern | Primitives used | Use case |
|---------|----------------|----------|
| Simple retry | Retry | Network calls, sensor reads |
| Breaker only | Circuit breaker | Protect against persistent failure |
| Retry + breaker | Both | Network with cascading failure protection |
| Rate limit | Rate limiter | API throttling, sensor polling |
| Full stack | All three | Mission-critical cloud publishing |
| FSM integration | Breaker + microfsm | State-dependent resilience |
| Spool integration | Retry + breaker + iotspool | Store-and-forward with resilience |
