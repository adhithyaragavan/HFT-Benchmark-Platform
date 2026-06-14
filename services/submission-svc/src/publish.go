package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"time"

	"github.com/twmb/franz-go/pkg/kgo"
)

type BuildRequest struct {
	SubmissionID string `json:"submission_id"`
	Language     string `json:"language"`
}

func main() {
	if len(os.Args) < 4 {
		fmt.Fprintf(os.Stderr, "Usage: publish <brokers> <submission_id> <language>\n")
		os.Exit(1)
	}

	brokers := os.Args[1]
	req := BuildRequest{
		SubmissionID: os.Args[2],
		Language:     os.Args[3],
	}

	b, _ := json.Marshal(req)

	cl, err := kgo.NewClient(
		kgo.SeedBrokers(brokers),
	)
	if err != nil {
		fmt.Fprintf(os.Stderr, "kgo.NewClient: %v\n", err)
		os.Exit(1)
	}
	defer cl.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	res := cl.ProduceSync(ctx, &kgo.Record{
		Topic: "build-requests",
		Value: b,
	})
	if err := res.FirstErr(); err != nil {
		fmt.Fprintf(os.Stderr, "ProduceSync: %v\n", err)
		os.Exit(1)
	}
}
