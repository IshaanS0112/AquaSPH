package testutil

import (
	"context"
	"time"

	"github.com/redis/go-redis/v9"
)

func pingRedis(u string) error {
	opts, err := redis.ParseURL(u)
	if err != nil {
		return err
	}
	c := redis.NewClient(opts)
	defer c.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	return c.Ping(ctx).Err()
}
