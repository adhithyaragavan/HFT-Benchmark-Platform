import os
import time
import redis

# Connect to the same Redis shared brain 🧠
r = redis.Redis(host='localhost', port=6379, decode_responses=True)

print("🖥️ Connecting to Live Exchange Dashboard...")

while True:
    # 1. Clear the terminal screen 🧹
    os.system('cls' if os.name == 'nt' else 'clear')
    
    # 2. Fetch and print the current market price 📈
    price = r.get("market:price")
    print("==========================================")
    print("🏆    LIVE TRADING HACKATHON LEADERBOARD   🏆")
    print("==========================================")
    print(f"📈 Current Market Price: ${price or '0.00'}")
    print("------------------------------------------")
    print(f"{'Rank':<6}{'Bot ID':<15}{'Net Worth':<12}")
    print("------------------------------------------")
    
    # 3. Pull all active bots from the Redis Set 📦
    bot_ids = r.smembers("active_bots")
    leaderboard_data = []
    
    # 4. Calculate Net Worth for each bot 💰
    for bot_id in bot_ids:
        account = r.hgetall(f"account:{bot_id}")
        if not account:
            continue
            
        cash = int(account.get("cash", 0))
        shares = int(account.get("shares", 0))
        
        # Net Worth = Cash + (Shares * Current Market Price)
        net_worth = cash + (shares * float(price or 0))
        
        leaderboard_data.append({
            "bot_id": bot_id,
            "net_worth": net_worth
        })
    
    # 5. Sort bots from highest net worth to lowest 🔄
    leaderboard_data.sort(key=lambda x: x["net_worth"], reverse=True)
    
    # 6. Print the ranked leaderboard 📊
    for rank, entry in enumerate(leaderboard_data, start=1):
        print(f"#{rank:<5}{entry['bot_id']:<15}${entry['net_worth']:.2f}")
        
    print("==========================================")
    print("🔄 Refreshing every second... Press Ctrl+C to exit.")
    
    # 7. Pause for 1 second ⏱️
    time.sleep(1)