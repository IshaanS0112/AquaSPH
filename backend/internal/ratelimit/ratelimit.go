// Package ratelimit implements per-tenant request rate limiting with the Generic Cell Rate
// Algorithm (GCRA) in a Redis Lua script.
package ratelimit

import (
	"context"
	"math"
	"time"

	"github.com/redis/go-redis/v9"
)

type Decision struct {
	Allowed    bool
	Limit      int           // burst size: requests allowed back to back
	Remaining  int           // requests allowed right now
	RetryAfter time.Duration // when denied: wait at least this long
	ResetAfter time.Duration // until the burst allowance is fully restored
}

type Limiter interface {
	Allow(ctx context.Context, key string, perMinute, burst int) (Decision, error)
}

// ARGV[1] = emission interval in microseconds (time per request at the sustained rate); ARGV[2]
// = burst.
var gcra = redis.NewScript(`
local emission = tonumber(ARGV[1])
local burst = tonumber(ARGV[2])
local t = redis.call('TIME')
local now = tonumber(t[1]) * 1000000 + tonumber(t[2])
local tat = tonumber(redis.call('GET', KEYS[1]))
if tat == nil or tat < now then tat = now end
local new_tat = tat + emission
local tolerance = emission * burst
local diff = new_tat - now
if diff > tolerance then
  return {0, 0, diff - tolerance, tat - now}
end
redis.call('SET', KEYS[1], new_tat, 'PX', math.ceil(diff / 1000) + 1)
return {1, math.floor((tolerance - diff) / emission), 0, diff}
`)

type Redis struct {
	client *redis.Client
	prefix string
}

func NewRedis(client *redis.Client, prefix string) *Redis {
	return &Redis{client: client, prefix: prefix}
}

func (r *Redis) Allow(ctx context.Context, key string, perMinute, burst int) (Decision, error) {
	emission := int64(math.Ceil(60e6 / float64(perMinute)))
	res, err := gcra.Run(ctx, r.client, []string{r.prefix + key}, emission, burst).Int64Slice()
	if err != nil {
		return Decision{}, err
	}
	return Decision{
		Allowed:    res[0] == 1,
		Limit:      burst,
		Remaining:  int(res[1]),
		RetryAfter: time.Duration(res[2]) * time.Microsecond,
		ResetAfter: time.Duration(res[3]) * time.Microsecond,
	}, nil
}
