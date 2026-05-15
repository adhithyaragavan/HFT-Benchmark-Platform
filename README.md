Distributed Systems Project for IICPC Summer Hackathon 2026

This repository contains the ongoing development of a Distributed Benchmarking and Hosting Platform designed to evaluate high-performance trading infrastructure.

📌 Project Overview
The goal is to build a resilient, decoupled system capable of hosting contestant-submitted matching engines and stress-testing them with a distributed fleet of trading bots.

Core Components in Development:

Submission Engine: Designing a secure pipeline to containerize and deploy C++ and Python binaries using Docker.

Distributed Bot Fleet: Architecting a scalable service in Python to simulate thousands of concurrent market participants via WebSockets.

Latency Ingester: Implementing a telemetry system to capture and validate p99 latencies and transaction throughput (TPS).
