import asyncio
import websockets
import json
import redis  
import time
import sys  

try:
    r = redis.Redis(host='localhost', port=6379, decode_responses=True)
    r.ping()
    print("🧠 Connected to Redis Shared Brain (Matching Engine Mode)!")
except redis.ConnectionError:
    print("❌ Could not connect to Redis. Make sure it's running!")
    exit()

async def exchange_handler(websocket):
    async for message in websocket:
        start_time = time.perf_counter()
        
        try:
            order = json.loads(message)
            bot_id = order.get("bot_id")
            side = order.get("side")        # BUY or SELL
            price = float(order.get("price"))
            qty = int(order.get("quantity"))
            
            # Unique ID for this specific order payload
            order_data_str = json.dumps({"bot_id": bot_id, "price": price, "qty": qty})
            
            trade_executed = False
            match_price = 0
            
            if side == "BUY":
                # 1. Peek at the lowest SELL order (index 0 to 0)
                best_sell = r.zrange("book:sell", 0, 0, withscores=True)
                
                if best_sell:
                    seller_data_raw, seller_price = best_sell[0]
                    # If our BUY price >= the lowest available SELL price -> MATCH!
                    if price >= seller_price:
                        # Attempt to remove the seller atomically to prevent double-matching
                        if r.zrem("book:sell", seller_data_raw) > 0:
                            trade_executed = True
                            match_price = seller_price
                            seller_data = json.loads(seller_data_raw)
                            # Award points to both matching bots in the leaderboard
                            r.incrby(f"score:{bot_id}", 1)
                            r.incrby(f"score:{seller_data['bot_id']}", 1)
                            
                # 2. No match found? Put the BUY order into the book (Negative Price Score)
                if not trade_executed:
                    r.zadd("book:buy", {order_data_str: -price})
                    
            elif side == "SELL":
                # 1. Peek at the highest BUY order (stored as lowest negative score)
                best_buy = r.zrange("book:buy", 0, 0, withscores=True)
                
                if best_buy:
                    buyer_data_raw, negative_buyer_price = best_buy[0]
                    buyer_price = -negative_buyer_price # Convert back to positive
                    
                    # If our SELL price <= the highest available BUY price -> MATCH!
                    if price <= buyer_price:
                        if r.zrem("book:buy", buyer_data_raw) > 0:
                            trade_executed = True
                            match_price = buyer_price
                            buyer_data = json.loads(buyer_data_raw)
                            r.incrby(f"score:{bot_id}", 1)
                            r.incrby(f"score:{buyer_data['bot_id']}", 1)
                            
                # 2. No match found? Put the SELL order into the book (Positive Price Score)
                if not trade_executed:
                    r.zadd("book:sell", {order_data_str: price})

            # Calculate server execution time
            proc_ms = (time.perf_counter() - start_time) * 1000
            
            # Send the response payload back to the bot
            if trade_executed:
                print(f"💥 TRADE MATCHED @ ${match_price:.2f} | Latency: {proc_ms:.4f}ms")
                ack = {"status": "FILLED", "match_price": match_price, "server_proc_ms": proc_ms}
            else:
                ack = {"status": "PENDING", "message": "Queued in book", "server_proc_ms": proc_ms}
                
            await websocket.send(json.dumps(ack))
            
        except Exception as e:
            print(f"⚠️ Error processing message: {e}")

async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"🏛️ Exchange Instance open on port {port}...")
    async with websockets.serve(exchange_handler, "localhost", port):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())