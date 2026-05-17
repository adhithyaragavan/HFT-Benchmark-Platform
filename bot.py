import asyncio
import websockets
import json
import time
import sys
import random

# Define all active server ports here
SERVER_PORTS = [8765, 8766] 

async def run_resilient_bot(bot_id, bot_index):
    # Dynamically distribute bots across whatever ports are in SERVER_PORTS
    port = SERVER_PORTS[bot_index % len(SERVER_PORTS)]
    uri = f"ws://localhost:{port}"
    
    while True:
        try:
            async with websockets.connect(uri) as websocket:
                print(f"✅ {bot_id} connected to port {port}.")
                
                while True:
                    # Determine side based on bot index (Even = BUY, Odd = SELL)
                    side = "BUY" if bot_index % 2 == 0 else "SELL"
                    
                    # Generate a realistic random price around $150.00
                    if side == "BUY":
                        price = round(random.uniform(149.00, 150.50), 2)
                    else:
                        price = round(random.uniform(149.50, 151.00), 2)
                    
                    # Prepare the dynamic order data
                    order_data = {
                        "bot_id": bot_id,
                        "symbol": "AAPL",
                        "side": side,      
                        "price": price,    
                        "quantity": 10
                    }
                    
                    # --- Latency Timing Starts Here ---
                    start_time = time.perf_counter() 
                    
                    await websocket.send(json.dumps(order_data))
                    response = await websocket.recv()
                    
                    end_time = time.perf_counter()
                    # ----------------------------------
                    
                    latency_ms = (end_time - start_time) * 1000
                    print(f"⏱️ {bot_id} ({side} @ ${price:.2f}) -> Port {port} | Latency: {latency_ms:.2f} ms")
                    
                    # Take a breath before sending the next order
                    await asyncio.sleep(1) 
                    
        except Exception as e:
            print(f"⚠️ {bot_id} disconnected from port {port}: {e}. Retrying in 2s...")
            await asyncio.sleep(2)

async def main():
    num_bots = 100
    tasks = []
    
    for i in range(num_bots):
        bot_id = f"bot_{i:03d}"
        tasks.append(run_resilient_bot(bot_id, i))
        
        # Tiny stagger to avoid overwhelming the network card at start
        await asyncio.sleep(0.01) 
    
    # Start all 100 bots
    await asyncio.gather(*tasks)

if __name__ == "__main__":
    asyncio.run(main())