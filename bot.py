import asyncio
import websockets
import json
import time
import sys

async def run_resilient_bot(bot_id, bot_index):
    # This bot will pick a port based on its index
    # (e.g., Bot 0 -> 8765, Bot 1 -> 8766, Bot 2 -> 8765...)
    port = 8765 + (bot_index % 2) 
    uri = f"ws://localhost:{port}"
    while True:
        try:
            # 1. Attempt to connect to the Exchange
            async with websockets.connect(uri) as websocket:
                print(f"✅ {bot_id} connected to port {port}.") # Updated to print the port
                
                while True:
                    # 2. Prepare the order data
                    order_data = {
                        "bot_id": bot_id,
                        "symbol": "AAPL",
                        "price": 150.25,
                        "quantity": 10
                    }
                    
                    # --- Latency Timing Starts Here ---
                    start_time = time.perf_counter() 
                    
                    await websocket.send(json.dumps(order_data))
                    response = await websocket.recv()
                    
                    end_time = time.perf_counter()
                    # ----------------------------------
                    
                    latency_ms = (end_time - start_time) * 1000
                    print(f"⏱️ {bot_id} (Port {port}) latency: {latency_ms:.2f} ms")
                    
                    # 4. Take a breath before the next order
                    await asyncio.sleep(1) 
                    
        except Exception as e:
            print(f"⚠️ {bot_id} disconnected from port {port}: {e}. Retrying in 2s...")
            await asyncio.sleep(2)

async def main():
    num_bots = 100
    tasks = []
    
    for i in range(num_bots):
        bot_id = f"bot_{i:03d}"
        # FIXED: Added 'i' as the second argument so the bot knows its index
        tasks.append(run_resilient_bot(bot_id, i))
        
        # Tiny stagger to avoid overwhelming the network card at start
        await asyncio.sleep(0.01) 
    
    # Start all 100 bots
    await asyncio.gather(*tasks)

if __name__ == "__main__":
    asyncio.run(main())