import asyncio
import websockets
import json
import redis  
import time
import sys
import uuid

try:
    r = redis.Redis(host='localhost', port=6379, decode_responses=True)
    r.ping()
    print("🧠 Connected to Redis Shared Brain (Risk-Protected Ledger Mode)!")
except redis.ConnectionError:
    print("❌ Could not connect to Redis. Make sure it's running!")
    exit()

async def exchange_handler(websocket):
    async for message in websocket:
        start_time = time.perf_counter()
        
        try:
            order = json.loads(message)
            bot_id = order.get("bot_id")
            side = order.get("side")
            price = float(order.get("price"))
            incoming_qty = int(order.get("quantity"))
            
            # 1. Ensure incoming bot has an initialized wallet 💰
            if not r.exists(f"account:{bot_id}"):
                r.hset(f"account:{bot_id}", mapping={"cash": 10000, "shares": 0})
                r.sadd("active_bots", bot_id)

            # 2. Pre-Flight Risk Validation Gates 🛡️
            if side == "BUY":
                max_cost = int(incoming_qty * price)
                current_cash = int(r.hget(f"account:{bot_id}", "cash") or 0)
                
                if current_cash < max_cost:
                    ack = {"status": "REJECTED", "message": "Insufficient funds"}
                    await websocket.send(json.dumps(ack))
                    continue  # Skip to the next message, keep connection alive
                    
            elif side == "SELL":
                current_shares = int(r.hget(f"account:{bot_id}", "shares") or 0)
                
                if current_shares < incoming_qty:
                    ack = {"status": "REJECTED", "message": "Insufficient shares"}
                    await websocket.send(json.dumps(ack))
                    continue  # Skip to the next message, keep connection alive

            # 3. Generate a unique ID for this order
            order_id = str(uuid.uuid4())
            
            # Save incoming order details in the Hash registry
            r.hset(f"order:{order_id}", mapping={
                "bot_id": bot_id, "side": side, "price": price, "qty": incoming_qty
            })
            
            trades_executed = []
            
            # ==============================
            # 🟢 BUY LOGIC
            # ==============================
            if side == "BUY":
                while incoming_qty > 0:
                    best_sell = r.zpopmin("book:sell")
                    if not best_sell:
                        break 
                        
                    resting_id, sell_price = best_sell[0]
                    
                    if price >= sell_price:
                        resting_order = r.hgetall(f"order:{resting_id}")
                        if not resting_order:
                            continue
                            
                        resting_qty = int(resting_order.get("qty", 0))
                        if resting_qty <= 0:
                            continue
                            
                        trade_qty = min(incoming_qty, resting_qty)
                        incoming_qty -= trade_qty
                        resting_qty -= trade_qty
                        
                        trades_executed.append({"price": sell_price, "qty": trade_qty})
                        
                        seller_id = resting_order['bot_id']
                        total_cost = int(trade_qty * sell_price)
                        
                        r.hincrby(f"account:{bot_id}", "cash", -total_cost)
                        r.hincrby(f"account:{bot_id}", "shares", trade_qty)
                        
                        r.hincrby(f"account:{seller_id}", "cash", total_cost)
                        r.hincrby(f"account:{seller_id}", "shares", -trade_qty)
                        
                        r.set("market:price", sell_price)
                        
                        if resting_qty > 0:
                            r.hset(f"order:{resting_id}", "qty", resting_qty)
                            r.zadd("book:sell", {resting_id: sell_price})
                    else:
                        r.zadd("book:sell", {resting_id: sell_price})
                        break
                        
                if incoming_qty > 0:
                    r.hset(f"order:{order_id}", "qty", incoming_qty)
                    r.zadd("book:buy", {order_id: -price})

            # ==============================
            # 🔴 SELL LOGIC
            # ==============================
            elif side == "SELL":
                while incoming_qty > 0:
                    best_buy = r.zpopmin("book:buy")
                    if not best_buy:
                        break
                        
                    resting_id, negative_buy_price = best_buy[0]
                    buy_price = abs(negative_buy_price)
                    
                    if price <= buy_price:
                        resting_order = r.hgetall(f"order:{resting_id}")
                        if not resting_order:
                            continue
                            
                        resting_qty = int(resting_order.get("qty", 0))
                        if resting_qty <= 0:
                            continue
                            
                        trade_qty = min(incoming_qty, resting_qty)
                        incoming_qty -= trade_qty
                        resting_qty -= trade_qty
                        
                        trades_executed.append({"price": buy_price, "qty": trade_qty})
                        
                        buyer_id = resting_order['bot_id']
                        total_cost = int(trade_qty * buy_price)
                        
                        r.hincrby(f"account:{buyer_id}", "cash", -total_cost)
                        r.hincrby(f"account:{buyer_id}", "shares", trade_qty)
                        
                        r.hincrby(f"account:{bot_id}", "cash", total_cost)
                        r.hincrby(f"account:{bot_id}", "shares", -trade_qty)
                        
                        r.set("market:price", buy_price)
                        
                        if resting_qty > 0:
                            r.hset(f"order:{resting_id}", "qty", resting_qty)
                            r.zadd("book:buy", {resting_id: negative_buy_price})
                    else:
                        r.zadd("book:buy", {resting_id: negative_buy_price})
                        break
                        
                if incoming_qty > 0:
                    r.hset(f"order:{order_id}", "qty", incoming_qty)
                    r.zadd("book:sell", {order_id: price})

            proc_ms = (time.perf_counter() - start_time) * 1000
            
            if trades_executed:
                print(f"💥 FILLED: {trades_executed} | Latency: {proc_ms:.4f}ms")
                ack = {"status": "FILLED_OR_PARTIAL", "trades": trades_executed, "remaining_qty": incoming_qty}
            else:
                ack = {"status": "PENDING", "message": "Queued in book", "remaining_qty": incoming_qty}
                
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