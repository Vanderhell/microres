# Patterns

## Retry only

Use retry when transient failures are expected and the caller can tolerate synchronous retries.

## Breaker around retry

Wrap a short retry loop inside a breaker when repeated end-to-end failure should open the circuit.

## Rate limit before work

Apply the limiter before expensive outbound work to reject bursts early.

## Two independent instances

Use separate instances when separate operations or resources must not share counters, jitter state, breaker state, or token buckets.

## External locking

Use one shared instance only when the caller can serialize access around the full API call and its callbacks.
