package worker_test

import (
	"testing"

	"github.com/prometheus/client_golang/prometheus"
	dto "github.com/prometheus/client_model/go"
)

func mustGather(t *testing.T, reg *prometheus.Registry) []*dto.MetricFamily {
	mfs, err := reg.Gather()
	if err != nil {
		t.Fatal(err)
	}
	return mfs
}
