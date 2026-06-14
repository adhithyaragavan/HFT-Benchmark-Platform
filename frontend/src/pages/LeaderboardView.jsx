import { useState, useEffect } from 'react'
import { useNavigate } from 'react-router-dom'
import { useWebSocket } from '../hooks/useWebSocket'



export function formatLatency(us) {
  if (!us && us !== 0) return '—'
  if (us < 1000) return `${us}μs`
  return `${(us / 1000).toFixed(1)}ms`
}

export function formatTPS(tps) {
  if (!tps && tps !== 0) return '—'
  if (tps >= 1000) return `${(tps / 1000).toFixed(1)}K`
  return tps.toString()
}

export function formatScore(score) {
  if (!score && score !== 0) return '—'
  return (score * 100).toFixed(1)
}

function getRankBadgeClass(rank) {
  if (rank === 1) return 'gold'
  if (rank === 2) return 'silver'
  if (rank === 3) return 'bronze'
  return 'default'
}

function getCorrectnessClass(c) {
  if (c >= 0.99) return 'high'
  if (c >= 0.95) return 'medium'
  return 'low'
}

function getLatencyClass(us) {
  if (!us && us !== 0) return ''
  if (us < 1000) return 'green'
  if (us <= 5000) return 'amber'
  return 'red'
}

export function Header({ connected }) {
  const navigate = useNavigate()
  const statusText = connected ? 'LIVE' : 'RECONNECTING...'
  const statusClass = connected ? 'connected' : 'disconnected'

  return (
    <header className="header">
      <div className="header-brand">
        <div className="header-logo">⚡</div>
        <div>
          <h1 className="header-title">Exchange Benchmark Arena</h1>
          <p className="header-subtitle">IICPC Summer Hackathon 2026 — Live Leaderboard</p>
        </div>
      </div>
      <div className="header-status" style={{ display: 'flex', alignItems: 'center', gap: '20px' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
          <span className={`status-dot ${statusClass}`} />
          <span className={`status-text ${statusClass}`}>
            {statusText}
          </span>
        </div>
        <button 
          onClick={() => navigate('/submit')}
          style={{
            background: 'var(--color-primary)',
            color: '#fff',
            border: 'none',
            padding: '8px 16px',
            borderRadius: '6px',
            fontWeight: 'bold',
            cursor: 'pointer',
            fontSize: '0.9rem'
          }}
        >
          Submit Your Exchange
        </button>
      </div>
    </header>
  )
}

function StatsBar({ entries }) {
  if (!entries || entries.length === 0) {
    return (
      <div className="stats-bar">
        {[...Array(4)].map((_, i) => (
          <div key={i} className="stat-card">
            <div className="skeleton-line" style={{width: '60%', height: 14, marginBottom: 8, background: 'var(--bg-tertiary)', borderRadius: 4}} />
            <div className="skeleton-line" style={{width: '40%', height: 28, marginBottom: 8, background: 'var(--bg-tertiary)', borderRadius: 4}} />
            <div className="skeleton-line" style={{width: '50%', height: 12, background: 'var(--bg-tertiary)', borderRadius: 4}} />
          </div>
        ))}
      </div>
    )
  }

  const bestLatency = Math.min(...entries.map(e => e.latency_p99_us || Infinity))
  const bestTPS = Math.max(...entries.map(e => e.max_tps || 0))
  const avgCorrectness = entries.reduce((sum, e) => sum + (e.correctness || 0), 0) / entries.length

  return (
    <div className="stats-bar">
      <div className="stat-card">
        <div className="stat-label">Teams Competing</div>
        <div className="stat-value blue">{entries.length}</div>
        <div className="stat-detail">active submissions</div>
      </div>
      <div className="stat-card">
        <div className="stat-label">Best P99 Latency</div>
        <div className="stat-value green">{formatLatency(bestLatency)}</div>
        <div className="stat-detail">order acknowledgment</div>
      </div>
      <div className="stat-card">
        <div className="stat-label">Peak Throughput</div>
        <div className="stat-value amber">{formatTPS(bestTPS)} TPS</div>
        <div className="stat-detail">transactions/second</div>
      </div>
      <div className="stat-card">
        <div className="stat-label">Avg Correctness</div>
        <div className="stat-value purple">{(avgCorrectness * 100).toFixed(1)}%</div>
        <div className="stat-detail">price-time priority</div>
      </div>
    </div>
  )
}

function LeaderboardTable({ entries }) {
  const navigate = useNavigate()

  const handleTeamAction = async (e, teamId, action) => {
    e.stopPropagation();
    try {
      const res = await fetch(`/api/v1/teams/${teamId}/${action}`, { method: 'POST' });
      if (!res.ok) {
        const err = await res.json();
        alert(`Failed to ${action} benchmark: ` + (err.error || 'Unknown error'));
      } else {
        // Success
      }
    } catch (err) {
      alert(`Network error while trying to ${action} benchmark`);
    }
  };

  const handleDeleteTeam = async (e, teamId) => {
    e.stopPropagation();
    if (!confirm(`Are you sure you want to delete the team '${teamId}' and all its submissions?`)) return;
    try {
      const res = await fetch(`/api/v1/teams/${teamId}`, { method: 'DELETE' });
      if (!res.ok) {
        const err = await res.json();
        alert(`Failed to delete team: ` + (err.error || 'Unknown error'));
      } else {
        // Success
      }
    } catch (err) {
      alert(`Network error while trying to delete team`);
    }
  };

  if (entries.length === 0) {
    return (
      <div className="empty-state">
        <div className="empty-icon">🏗️</div>
        <div className="empty-text">No submissions yet</div>
        <div className="empty-subtext">Teams are still building their exchanges. Stay tuned!</div>
      </div>
    )
  }

  return (
    <table className="leaderboard-table">
      <thead>
        <tr>
          <th style={{ width: 60 }}>Rank</th>
          <th>Team</th>
          <th>P50</th>
          <th>P90</th>
          <th>P99</th>
          <th>Max TPS</th>
          <th>Correctness</th>
          <th>Score</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        {entries.map((entry, i) => (
          <tr 
            key={entry.team_id} 
            className={`leaderboard-row rank-${entry.rank}`}
            onClick={() => navigate(`/run/${entry.team_id}`)}
            style={{ cursor: 'pointer' }}
            title="Click to view full benchmark report"
          >
            <td>
              <span className={`rank-badge ${getRankBadgeClass(entry.rank)}`}>
                {entry.rank}
              </span>
            </td>
            <td>
              <span className="team-name">{entry.team_id}</span>
            </td>
            <td>
              <span className={`metric-cell latency ${getLatencyClass(entry.latency_p50_us)}`}>
                {formatLatency(entry.latency_p50_us)}
              </span>
            </td>
            <td>
              <span className={`metric-cell latency ${getLatencyClass(entry.latency_p90_us)}`}>
                {formatLatency(entry.latency_p90_us)}
              </span>
            </td>
            <td>
              <span className={`metric-cell latency ${getLatencyClass(entry.latency_p99_us)}`}>
                {formatLatency(entry.latency_p99_us)}
              </span>
            </td>
            <td>
              <span className="metric-cell throughput">
                {formatTPS(entry.max_tps)}
              </span>
            </td>
            <td>
              <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                <div className="correctness-bar">
                  <div
                    className={`correctness-fill ${getCorrectnessClass(entry.correctness)}`}
                    style={{ width: `${(entry.correctness || 0) * 100}%` }}
                  />
                </div>
                <span className="metric-cell correctness">
                  {((entry.correctness || 0) * 100).toFixed(1)}%
                </span>
              </div>
            </td>
            <td>
              <span className="score-badge">
                {formatScore(entry.composite_score)}
              </span>
            </td>
            <td>
              <div style={{ display: 'flex', gap: '8px' }}>
                <button
                  onClick={(e) => handleTeamAction(e, entry.team_id, 'start')}
                  style={{
                    background: 'var(--color-primary)', border: 'none', color: '#fff',
                    borderRadius: '4px', padding: '4px 8px', cursor: 'pointer', fontSize: '0.8rem'
                  }}
                  title="Start Benchmark"
                >
                  ▶️
                </button>
                <button
                  onClick={(e) => handleTeamAction(e, entry.team_id, 'stop')}
                  style={{
                    background: 'var(--color-amber)', border: 'none', color: '#fff',
                    borderRadius: '4px', padding: '4px 8px', cursor: 'pointer', fontSize: '0.8rem'
                  }}
                  title="Stop Benchmark"
                >
                  ⏹️
                </button>
                <button
                  onClick={(e) => handleDeleteTeam(e, entry.team_id)}
                  style={{
                    background: 'var(--color-red)', border: 'none', color: '#fff',
                    borderRadius: '4px', padding: '4px 8px', cursor: 'pointer', fontSize: '0.8rem'
                  }}
                  title="Delete Team"
                >
                  🗑️
                </button>
              </div>
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

function PhaseBanner({ phaseData }) {
  const phases = ['WARMUP', 'RAMP', 'PEAK', 'BURST', 'COOLDOWN']
  const [remaining, setRemaining] = useState(0)

  useEffect(() => {
    if (phaseData) setRemaining(phaseData.remaining_seconds)
  }, [phaseData])

  useEffect(() => {
    const interval = setInterval(() => {
      setRemaining(prev => Math.max(0, prev - 1))
    }, 1000)
    return () => clearInterval(interval)
  }, [])
  
  if (!phaseData) {
    return (
      <div className="phase-banner awaiting" style={{ padding: '12px 20px', background: 'var(--bg-secondary)', borderRadius: 8, margin: '20px 0', textAlign: 'center', color: 'var(--text-muted)' }}>
        <span style={{ marginRight: 16 }}>Awaiting test start:</span>
        {phases.map((p, i) => (
          <span key={p} style={{ opacity: 0.5 }}>
            {p}{i < phases.length - 1 ? ' → ' : ''}
          </span>
        ))}
      </div>
    )
  }

  const formatTime = (secs) => {
    const m = Math.floor(secs / 60)
    const s = secs % 60
    return `${m}:${s.toString().padStart(2, '0')}`
  }

  return (
    <div className="phase-banner active" style={{ padding: '12px 20px', background: 'var(--bg-secondary)', borderRadius: 8, margin: '20px 0', display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
      <div className="phases" style={{ display: 'flex', gap: '8px' }}>
        {phases.map((p, i) => {
          const isActive = p === phaseData.current
          return (
            <span key={p} style={{ 
              color: isActive ? 'var(--text-primary)' : 'var(--text-muted)',
              fontWeight: isActive ? 'bold' : 'normal',
              opacity: isActive ? 1 : 0.5
            }}>
              {p}{i < phases.length - 1 ? ' →' : ''}
            </span>
          )
        })}
      </div>
      <div className="phase-timer" style={{ fontFamily: 'monospace', fontSize: '1.1em', color: 'var(--color-amber)' }}>
        {phaseData.current} {formatTime(remaining)} remaining
      </div>
    </div>
  )
}

export default function LeaderboardView() {
  const wsUrl = `${window.location.protocol === 'https:' ? 'wss:' : 'ws:'}//${window.location.host}/api/v1/leaderboard/stream`
  const { data, connected, lastMessageTime } = useWebSocket(wsUrl)
  const [entries, setEntries] = useState([])
  const [phaseData, setPhaseData] = useState(null)
  const [timeAgo, setTimeAgo] = useState(0)

  useEffect(() => {
    if (data) {
      if (data.type === 'phase') {
        setPhaseData(data)
      } else if (data.entries && data.entries.length > 0) {
        setEntries(data.entries)
      }
    }
  }, [data])

  useEffect(() => {
    const interval = setInterval(() => {
      if (lastMessageTime) {
        setTimeAgo(Math.floor((Date.now() - lastMessageTime) / 1000))
      }
    }, 1000)
    return () => clearInterval(interval)
  }, [lastMessageTime])

  const handleGlobalAction = async (action) => {
    if (entries.length === 0) return;
    try {
      const promises = entries.map(entry => 
        fetch(`/api/v1/teams/${entry.team_id}/${action}`, { method: 'POST' })
          .then(res => res.ok ? Promise.resolve() : Promise.reject())
      );
      const results = await Promise.allSettled(promises);
      const successes = results.filter(r => r.status === 'fulfilled').length;
      alert(`Successfully ${action}ed benchmark for ${successes}/${entries.length} teams.`);
    } catch (err) {
      alert(`Network error while executing ${action} all`);
    }
  };

  return (
    <div className="app-container">
      <Header connected={connected} />
      <StatsBar entries={entries} />
      <PhaseBanner phaseData={phaseData} />
      <section className="leaderboard-section">
        <div className="section-header">
          <h2 className="section-title" style={{ display: 'flex', alignItems: 'center', gap: '16px' }}>
            <span>
              <span className="icon">🏆</span>
              Live Rankings
            </span>
            <div style={{ display: 'flex', gap: '8px' }}>
              <button 
                onClick={() => handleGlobalAction('start')}
                style={{
                  background: 'var(--color-primary)', color: '#fff', border: 'none',
                  padding: '6px 12px', borderRadius: '4px', cursor: 'pointer', fontSize: '0.9rem', fontWeight: 'bold'
                }}
              >
                ▶️ Start All
              </button>
              <button 
                onClick={() => handleGlobalAction('stop')}
                style={{
                  background: 'var(--color-red)', color: '#fff', border: 'none',
                  padding: '6px 12px', borderRadius: '4px', cursor: 'pointer', fontSize: '0.9rem', fontWeight: 'bold'
                }}
              >
                ⏹️ Stop All
              </button>
            </div>
          </h2>
          <span className={`status-text ${timeAgo > 10 ? 'stale' : ''}`} style={timeAgo > 10 ? {color: 'var(--color-red)'} : {}}>
            {timeAgo > 10 && <span className="status-dot disconnected" style={{marginRight: 6}}/>}
            {timeAgo > 10 ? 'STALE ' : ''}
            {lastMessageTime ? `Updated ${timeAgo}s ago` : 'Awaiting first update'}
          </span>
        </div>
        <LeaderboardTable entries={entries} />
      </section>
    </div>
  )
}
