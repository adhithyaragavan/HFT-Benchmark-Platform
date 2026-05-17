import redis
import time
import os
import json

def clear_screen():
    os.system('cls' if os.name == 'nt' else 'clear')

def main():
    try:
        r = redis.Redis(host='localhost', port=6379, decode_responses=True)
        r.ping()
    except redis.ConnectionError:
        print("❌ Dashboard cannot connect to Redis!")
        return

    while True:
        try:
            # 1. Fetch top 5 Bids (BUYS) and top 5 Asks (SELLS)
            raw_bids = r.zrange("book:buy", 0, 4, withscores=True)
            raw_asks = r.zrange("book:sell", 0, 4, withscores=True)
            
            # 2. Extract out just the prices using abs() for the buy bids
            bids = [abs(score) for _, score in raw_bids]
            asks = [score for _, score in raw_asks]
            
            clear_screen()
            print("==================================================")
            print("       🏛️  DISTRIBUTED ORDER BOOK DEPTH 🏛️        ")
            print("==================================================")
            print(f" {'  BIDS (BUY)  ':^22} | {'  ASKS (SELL) ':^22} ")
            print("--------------------------------------------------")
            
            # Display rows side by side
            for i in range(5):
                bid_str = f"${bids[i]:.2f}" if i < len(bids) else "---"
                ask_str = f"${asks[i]:.2f}" if i < len(asks) else "---"
                # Highlight the top row (best available prices) with arrows
                if i == 0 and (bids or asks):
                    print(f" 🟢 Best: {bid_str:<11} |  🔴 Best: {ask_str:<11}")
                else:
                    print(f"       {bid_str:<11} |         {ask_str:<11}")
                    
            print("--------------------------------------------------")
            
            # 3. Calculate and display the spread
            if bids and asks:
                spread = asks[0] - bids[0]
                print(f" 🏁 Current Market Spread: ${spread:.2f}")
            else:
                print(" 🏁 Current Market Spread: Waiting for liquidity...")
                
            print("==================================================")
            
            # Count remaining items in queue
            total_bids = r.zcard("book:buy")
            total_asks = r.zcard("book:sell")
            print(f" Resting Orders Queue -> Bids: {total_bids} | Asks: {total_asks}")
            print(" 🔄 Refreshing... Press Ctrl+C to close.")
            
            time.sleep(0.3)
            
        except KeyboardInterrupt:
            print("\n👋 Dashboard closed.")
            break
        except Exception as e:
            print(f"⚠️ Dashboard error: {e}")
            time.sleep(2)

if __name__ == "__main__":
    main()