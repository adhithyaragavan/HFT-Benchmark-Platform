import { useState, useEffect, useCallback, useRef } from 'react'

/**
 * Custom hook for WebSocket connection to the leaderboard service.
 * Handles connection, reconnection, and message parsing.
 */
export function useWebSocket(url) {
  const [data, setData] = useState(null)
  const [connected, setConnected] = useState(false)
  const [lastMessageTime, setLastMessageTime] = useState(null)
  const wsRef = useRef(null)
  const reconnectTimer = useRef(null)
  const backoffRef = useRef(1000)

  const connect = useCallback(() => {
    try {
      const ws = new WebSocket(url)
      wsRef.current = ws

      ws.onopen = () => {
        setConnected(true)
        backoffRef.current = 1000 // Reset backoff on success
        console.log('[WS] Connected to', url)
      }

      ws.onmessage = (event) => {
        try {
          const parsed = JSON.parse(event.data)
          setData(parsed)
          setLastMessageTime(Date.now())
        } catch (e) {
          console.error('[WS] Parse error:', e)
        }
      }

      ws.onclose = (event) => {
        setConnected(false)
        console.log('[WS] Disconnected, code:', event.code)
        // Exponential backoff
        reconnectTimer.current = setTimeout(connect, backoffRef.current)
        backoffRef.current = Math.min(backoffRef.current * 2, 30000)
      }

      ws.onerror = (err) => {
        console.error('[WS] Error:', err)
        ws.close()
      }
    } catch (e) {
      console.error('[WS] Connection failed:', e)
      reconnectTimer.current = setTimeout(connect, backoffRef.current)
      backoffRef.current = Math.min(backoffRef.current * 2, 30000)
    }
  }, [url])

  useEffect(() => {
    connect()
    return () => {
      if (wsRef.current) wsRef.current.close()
      if (reconnectTimer.current) clearTimeout(reconnectTimer.current)
    }
  }, [connect])

  const send = useCallback((msg) => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      wsRef.current.send(typeof msg === 'string' ? msg : JSON.stringify(msg))
    }
  }, [])

  return { data, connected, lastMessageTime, send }
}
