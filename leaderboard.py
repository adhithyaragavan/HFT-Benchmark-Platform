import redis
import time
import os


def clear_screen():
    # Clears the terminal screen for a smooth visual update
    os.system('cls' if os.name == 'nt' else 'clear')

def main():
    try:
        # Connect to the same Redis instance
        r = redis.Redis(host='localhost', port=6379, decode_responses=True)
        r.ping()
    except redis.ConnectionError:
        print("❌ Leaderboard cannot connect to Redis. Make sure Redis is running!")
        return

    print("📊 Real-Time Exchange Leaderboard Initialized...")
    time.sleep(1)

    while True:
        try:
            # 1. Fetch all keys that store bot scores
            keys = r.keys("score:bot_*")
            
            leaderboard_data = []
            for key in keys:
                bot_id = key.split(":")[1] # Extracts 'bot_001' from 'score:bot_001'
                score = int(r.get(key) or 0)
                leaderboard_data.append((bot_id, score))
            
            # 2. Sort the leaderboard by score descending
            leaderboard_data.sort(key=lambda x: x[1], reverse=True)
            
            # 3. Render the UI
            clear_screen()
            print("========================================")
            print("       🏆 HACKATHON LIVE LEADERBOARD 🏆  ")
            print("========================================")
            print(f" {'Rank':<6} | {'Bot ID':<12} | {'Total Orders Placed':<15}")
            print("----------------------------------------")
            
            # Display the Top 10 bots
            for rank, (bot_id, score) in enumerate(leaderboard_data[:10], start=1):
                print(f" #{rank:<4} | {bot_id:<12} | {score:<15,}")
                
            print("----------------------------------------")
            print(f"Active Bots Tracking: {len(leaderboard_data)}")
            print("🔄 Updating every 0.5 seconds... Press Ctrl+C to exit.")
            
            # Refresh rate
            time.sleep(0.5)
            
        except KeyboardInterrupt:
            print("\n👋 Leaderboard closed.")
            break
        except Exception as e:
            print(f"⚠️ Error updating leaderboard: {e}")
            time.sleep(2)

if __name__ == "__main__":
    main()