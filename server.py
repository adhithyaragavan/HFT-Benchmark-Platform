import asyncio
import websockets
import json
import redis  
import time
import sys

# 1. Connect to the Shared Brain (Redis)
# This allows multiple servers to share the same leaderboard
try:
    r = redis.Redis(host='localhost', port=6379, decode_responses=True)
    # Check if connection works
    r.ping()
    print("🧠 Connected to Redis Shared Brain!")
except redis.ConnectionError:
    print("❌ Could not connect to Redis. Make sure it's running!")
    exit()

async def exchange_handler(websocket):
    async for message in websocket:
        # Start timing internal processing
        start_time = time.perf_counter()
        
        try:
            order = json.loads(message)
            bot_id = order.get("bot_id", "anonymous")
            
            # 2. ATOMIC UPDATE: Increment the bot's score in Redis
            # 'incrby' is thread-safe and prevents race conditions
            new_total = r.incrby(f"score:{bot_id}", 1)
            
            # Calculate how long the 'Brain' took to think
            end_time = time.perf_counter()
            proc_ms = (end_time - start_time) * 1000
            
            # Print status update
            print(f"📦 {bot_id} | Total: {new_total} | Internal Latency: {proc_ms:.4f}ms")
            
            # 3. Send back the updated total so the bot knows its rank
            ack = {
                "status": "SUCCESS", 
                "total_orders": new_total,
                "server_proc_ms": proc_ms
            }
            await websocket.send(json.dumps(ack))
            
        except Exception as e:
            print(f"⚠️ Error processing message: {e}")

async def main():
    # Get port from command line, or default to 8765
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765

    print(f"🏛️ Exchange Instance open on port {port}...")
    async with websockets.serve(exchange_handler, "localhost", port):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())