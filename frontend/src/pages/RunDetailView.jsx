import { useState, useEffect } from 'react'
import { useParams, Link, useNavigate } from 'react-router-dom'
import { BarChart, Bar, XAxis, YAxis, Tooltip, ResponsiveContainer, CartesianGrid, LineChart, Line, ReferenceLine } from 'recharts'
import { formatLatency, formatTPS, Header } from './LeaderboardView'
import { useWebSocket } from '../hooks/useWebSocket'

export default function RunDetailView() {
  const { runId } = useParams()
  const wsUrl = `${window.location.protocol === 'https:' ? 'wss:' : 'ws:'}//${window.location.host}/api/v1/leaderboard/stream`
  const { data, connected, send } = useWebSocket(wsUrl)
  
  const [metrics, setMetrics] = useState(null)
  const [histogram, setHistogram] = useState(null)
  const [error, setError] = useState(!runId ? "Please navigate here from the leaderboard. No run ID provided." : null)
  const navigate = useNavigate()

  const handleDeleteTeam = async () => {
    if (!window.confirm(`Are you sure you want to delete team ${runId}? This action cannot be undone.`)) {
      return
    }
    
    try {
      const res = await fetch(`/api/v1/teams/${runId}`, {
        method: 'DELETE'
      })
      if (res.ok) {
        navigate('/')
      } else {
        const errorData = await res.json()
        alert('Failed to delete team: ' + (errorData.error || 'Unknown error'))
      }
    } catch (err) {
      alert('Network error while deleting team')
    }
  }

  useEffect(() => {
    if (connected && runId) {
      send({ type: 'subscribe_detail', submission_id: runId })
    }
  }, [connected, runId, send])

  useEffect(() => {
    if (data && data.type === 'detail_update' && data.submission_id === runId) {
      setMetrics({
        latency_p50_us: data.latency_p50_us,
        latency_p90_us: data.latency_p90_us,
        latency_p99_us: data.latency_p99_us,
        current_tps: data.current_tps,
      })
      if (data.buckets) {
        const formatted = data.buckets.map(b => ({
          range: `${formatLatency(b.min_value_us)} - ${formatLatency(b.max_value_us)}`,
          count: b.count,
          min: b.min_value_us,
          max: b.max_value_us
        })).filter(b => b.count > 0)
        setHistogram(formatted)
      }
    }
  }, [data, runId])
  return (
    <div className="app-container">
      <Header connected={connected} />
      
      <div style={{ marginBottom: '20px' }}>
        <Link to="/" style={{ color: 'var(--text-secondary)', textDecoration: 'none' }}>
          &larr; Back to Leaderboard
        </Link>
      </div>

      <div className="section-header" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <h2 className="section-title">Team: {runId}</h2>
        <button 
          onClick={handleDeleteTeam}
          style={{
            padding: '8px 16px',
            background: 'var(--color-red)',
            color: '#fff',
            border: 'none',
            borderRadius: '6px',
            fontSize: '0.9rem',
            fontWeight: 'bold',
            cursor: 'pointer',
            opacity: 0.9
          }}
        >
          🗑️ Delete Team
        </button>
      </div>

      {error ? (
        <div className="empty-state">
          <div className="empty-text">Not Found</div>
          <div className="empty-subtext">{error}</div>
        </div>
      ) : (
        <>
          <div className="stats-bar" style={{ marginBottom: '40px' }}>
             <div className="stat-card">
               <div className="stat-label">P50 Latency</div>
               <div className="stat-value blue">{formatLatency(metrics?.latency_p50_us)}</div>
             </div>
             <div className="stat-card">
               <div className="stat-label">P90 Latency</div>
               <div className="stat-value amber">{formatLatency(metrics?.latency_p90_us)}</div>
             </div>
             <div className="stat-card">
               <div className="stat-label">P99 Latency</div>
               <div className="stat-value green">{formatLatency(metrics?.latency_p99_us)}</div>
             </div>
             <div className="stat-card">
               <div className="stat-label">Current TPS</div>
               <div className="stat-value purple">{formatTPS(metrics?.current_tps)}</div>
             </div>
          </div>

          <div className="chart-container" style={{ background: 'var(--bg-secondary)', padding: '24px', borderRadius: '12px', border: '1px solid var(--border)' }}>
            <h3 style={{ margin: '0 0 24px 0', fontSize: '1.2rem' }}>Latency Distribution</h3>
            {histogram && histogram.length > 0 ? (
              <div style={{ width: '100%', height: 350 }}>
                <ResponsiveContainer>
                  <BarChart data={histogram} margin={{ top: 10, right: 30, left: 0, bottom: 30 }}>
                    <CartesianGrid strokeDasharray="3 3" stroke="#333" vertical={false} />
                    <XAxis 
                      dataKey="range" 
                      stroke="#888" 
                      tick={{ fill: '#888', fontSize: 12 }}
                      angle={-45}
                      textAnchor="end"
                    />
                    <YAxis 
                      stroke="#888" 
                      tick={{ fill: '#888', fontSize: 12 }}
                      label={{ value: 'Orders', angle: -90, position: 'insideLeft', fill: '#888' }}
                    />
                    <Tooltip 
                      contentStyle={{ background: '#111', border: '1px solid #333', borderRadius: '8px' }}
                      itemStyle={{ color: '#fff' }}
                      cursor={{ fill: 'rgba(255, 255, 255, 0.05)' }}
                    />
                    <Bar dataKey="count" fill="var(--color-primary)" radius={[4, 4, 0, 0]} />
                    {metrics?.latency_p99_us && (
                      <ReferenceLine 
                         x={histogram.find(b => metrics.latency_p99_us >= b.min && metrics.latency_p99_us <= b.max)?.range} 
                         stroke="var(--color-success)" 
                         strokeDasharray="3 3"
                         label={{ position: 'top', value: 'p99', fill: 'var(--color-success)', fontSize: 12 }} 
                      />
                    )}
                  </BarChart>
                </ResponsiveContainer>
              </div>
            ) : (
              <div className="empty-state" style={{ padding: '40px' }}>Collecting latency buckets...</div>
            )}
          </div>
        </>
      )}
    </div>
  )
}
