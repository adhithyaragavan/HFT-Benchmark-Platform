import asyncio
import websockets
import json
import time
import sys
import random

# Define all active server ports here
SERVER_PORTS = [8765,8766] 

async def run_resilient_bot(bot_id, bot_index):
    port = SERVER_PORTS[bot_index % len(SERVER_PORTS)]
    uri = f"ws://localhost:{port}"
    
    while True:
        try:
            async with websockets.connect(uri) as websocket:
                print(f"✅ {bot_id} connected to port {port}.")
                
                while True:
                    side = "BUY" if bot_index % 2 == 0 else "SELL"
                    
                    if side == "BUY":
                        price = round(random.uniform(149.00, 150.50), 2)
                    else:
                        price = round(random.uniform(149.50, 151.00), 2)
                    
                    order_data = {
                        "bot_id": bot_id,
                        "symbol": "AAPL",
                        "side": side,      
                        "price": price,    
                        "quantity": random.choice([10, 20, 30, 40, 50])
                    }
                    
                    start_time = time.perf_counter() 
                    await websocket.send(json.dumps(order_data))
                    
                    # Receive the ledger engine's breakdown 📨
                    response = await websocket.recv()
                    end_time = time.perf_counter()
                    
                    latency_ms = (end_time - start_time) * 1000
                    ack = json.loads(response)
                    
                    # Display response status based on matching outcome 💥/⏳
                    if ack.get("status") == "FILLED_OR_PARTIAL":
                        print(f"💥 {bot_id} | {side} FILLED | Trades: {ack['trades']} | Latency: {latency_ms:.2f} ms")
                    else:
                        print(f"⏳ {bot_id} | {side} QUEUED | {ack['message']} | Latency: {latency_ms:.2f} ms")
                    
                    await asyncio.sleep(1) 
                    
        except Exception as e:
            print(f"⚠️ {bot_id} disconnected from port {port}: {e}. Retrying in 2s...")
            await asyncio.sleep(2)

async def main():
    num_bots = 10  # Reduced default for local testing; scale back up to 100 as needed
    tasks = []
    
    for i in range(num_bots):
        bot_id = f"bot_{i:03d}"
        tasks.append(run_resilient_bot(bot_id, i))
        await asyncio.sleep(0.01) 
    
    await asyncio.gather(*tasks)

if __name__ == "__main__":
    asyncio.run(main())