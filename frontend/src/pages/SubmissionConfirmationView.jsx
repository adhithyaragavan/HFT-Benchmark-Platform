import { useState, useEffect } from 'react'
import { useSearchParams, Link, useNavigate } from 'react-router-dom'

export default function SubmissionConfirmationView() {
  const [searchParams] = useSearchParams()
  const id = searchParams.get('id')
  const team = searchParams.get('team')
  const navigate = useNavigate()

  const [status, setStatus] = useState('uploaded')
  const [errorMsg, setErrorMsg] = useState(null)

  const STAGES = ['uploaded', 'building', 'ready', 'testing', 'complete']

  useEffect(() => {
    if (!id) return

    const pollStatus = async () => {
      try {
        const res = await fetch(`/api/submission/${id}/status`)
        if (res.ok) {
          const responseData = await res.json()
          if (responseData.data) {
            if (responseData.data.status) {
              setStatus(responseData.data.status.toLowerCase())
            }
            if (responseData.data.error) {
              setErrorMsg(responseData.data.error)
            } else {
              setErrorMsg(null)
            }
          }
        }
      } catch (err) {
        // silently ignore network errors on polling to allow reconnects
      }
    }

    pollStatus()
    const interval = setInterval(pollStatus, 3000)

    // Only stop polling when we reach a terminal state
    if (status === 'complete' || status === 'failed' || (status !== 'testing' && status !== 'ready' && errorMsg)) {
      clearInterval(interval)
    }

    return () => clearInterval(interval)
  }, [id, status, errorMsg])

  const handleStartBenchmark = async () => {
    try {
      const res = await fetch(`/api/v1/submissions/${id}/start`, {
        method: 'POST',
      });
      if (res.ok) {
        setStatus('testing');
      } else {
        const errorData = await res.json();
        setErrorMsg(errorData.error || 'Failed to start benchmark');
      }
    } catch (err) {
      setErrorMsg('Network error while starting benchmark');
    }
  };

  const handleStopBenchmark = async () => {
    try {
      const res = await fetch(`/api/v1/submissions/${id}/stop`, {
        method: 'POST',
      });
      if (res.ok) {
        setStatus('ready');
      } else {
        const errorData = await res.json();
        setErrorMsg(errorData.error || 'Failed to stop benchmark');
      }
    } catch (err) {
      setErrorMsg('Network error while stopping benchmark');
    }
  };

  if (!id) {
    return (
      <div className="app-container" style={{ textAlign: 'center', padding: '100px 20px' }}>
        <h2>Invalid Confirmation Link</h2>
        <p style={{ color: 'var(--text-muted)' }}>No submission ID found in URL.</p>
        <button onClick={() => navigate('/')} style={{ marginTop: '20px', padding: '10px 20px', background: 'var(--color-primary)', border: 'none', borderRadius: '6px', color: '#fff', cursor: 'pointer' }}>Return to Leaderboard</button>
      </div>
    )
  }

  const getStageState = (stageName) => {
    if (errorMsg && status === stageName) return 'error'
    const stageIdx = STAGES.indexOf(stageName)
    const currentIdx = STAGES.indexOf(status)
    if (stageIdx < currentIdx) return 'complete'
    if (stageIdx === currentIdx) return 'active'
    return 'pending'
  }

  const stagesToRender = ['uploaded', 'building', 'ready', 'testing']

  return (
    <div className="app-container">
      <header className="header" style={{ justifyContent: 'center' }}>
        <div className="header-brand">
          <div className="header-logo">⚡</div>
          <div>
            <h1 className="header-title">Exchange Benchmark Arena</h1>
            <p className="header-subtitle">Submission Status</p>
          </div>
        </div>
      </header>

      <div style={{ maxWidth: '800px', margin: '60px auto', textAlign: 'center' }}>
        <div style={{ fontSize: '4rem', marginBottom: '20px' }}>✅</div>
        <h2 style={{ fontSize: '2rem', marginBottom: '10px' }}>Upload Successful!</h2>
        <p style={{ fontSize: '1.2rem', color: 'var(--text-secondary)', marginBottom: '30px' }}>
          Team <strong style={{ color: 'var(--text-primary)' }}>{team || 'Unknown'}</strong>
        </p>

        <div style={{ background: 'var(--bg-secondary)', padding: '24px', borderRadius: '12px', border: '1px solid var(--border)', marginBottom: '40px' }}>
          <div style={{ color: 'var(--text-muted)', marginBottom: '8px', textTransform: 'uppercase', fontSize: '0.85rem', fontWeight: 'bold' }}>Submission ID</div>
          <div style={{ fontFamily: 'monospace', fontSize: '1.2rem', color: 'var(--color-primary)', background: 'var(--bg-tertiary)', padding: '16px', borderRadius: '8px', userSelect: 'all' }}>
            {id}
          </div>
        </div>

        <div style={{ marginBottom: '40px' }}>
          <h3 style={{ marginBottom: '24px', color: 'var(--text-secondary)' }}>Live Pipeline Status</h3>
          <div style={{ display: 'flex', justifyContent: 'space-between', position: 'relative' }}>
            
            {/* Connecting line */}
            <div style={{ position: 'absolute', top: '24px', left: '10%', right: '10%', height: '4px', background: 'var(--bg-tertiary)', zIndex: 0 }} />
            <div style={{ position: 'absolute', top: '24px', left: '10%', right: `${100 - (STAGES.indexOf(status) / 3) * 80}%`, height: '4px', background: errorMsg ? 'var(--color-red)' : 'var(--color-primary)', zIndex: 0, transition: 'right 0.5s' }} />

            {stagesToRender.map((stage) => {
              const state = getStageState(stage)
              let circleBg = 'var(--bg-tertiary)'
              let circleColor = 'var(--text-muted)'
              let circleBorder = '4px solid var(--bg-primary)'
              
              if (state === 'complete') {
                circleBg = 'var(--color-primary)'
                circleColor = '#fff'
              } else if (state === 'active') {
                circleBg = 'var(--bg-secondary)'
                circleColor = 'var(--color-primary)'
                circleBorder = '4px solid var(--color-primary)'
              } else if (state === 'error') {
                circleBg = 'var(--color-red)'
                circleColor = '#fff'
                circleBorder = '4px solid var(--bg-primary)'
              }

              return (
                <div key={stage} style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', zIndex: 1, width: '25%' }}>
                  <div style={{ 
                    width: '48px', height: '48px', borderRadius: '50%', background: circleBg, border: circleBorder,
                    display: 'flex', alignItems: 'center', justifyContent: 'center', marginBottom: '12px',
                    fontWeight: 'bold', color: circleColor, transition: 'all 0.3s'
                  }}>
                    {state === 'complete' ? '✓' : state === 'error' ? '!' : STAGES.indexOf(stage) + 1}
                  </div>
                  <div style={{ fontWeight: 'bold', color: state === 'pending' ? 'var(--text-muted)' : (state === 'error' ? 'var(--color-red)' : 'var(--text-primary)'), textTransform: 'capitalize' }}>
                    {stage}
                  </div>
                </div>
              )
            })}
          </div>

          {errorMsg && (
            <div style={{ marginTop: '30px', padding: '16px', background: 'rgba(239, 68, 68, 0.1)', border: '1px solid var(--color-red)', borderRadius: '8px', color: 'var(--color-red)', textAlign: 'left' }}>
              <strong>Pipeline Error at {status.toUpperCase()}:</strong> {errorMsg}
            </div>
          )}
        </div>

        {status === 'ready' && !errorMsg && (
          <div style={{ marginTop: '40px' }}>
            <button 
              onClick={handleStartBenchmark}
              style={{
                padding: '16px 32px',
                background: 'var(--color-primary)',
                color: '#fff',
                border: 'none',
                borderRadius: '8px',
                fontSize: '1.2rem',
                fontWeight: 'bold',
                cursor: 'pointer',
                boxShadow: '0 4px 12px rgba(59, 130, 246, 0.3)'
              }}
            >
              Start Benchmark
            </button>
          </div>
        )}

        {(status === 'testing' || status === 'complete') && !errorMsg && (
          <div style={{ marginTop: '40px', display: 'flex', justifyContent: 'center', gap: '20px' }}>
            <button 
              onClick={() => navigate('/')}
              style={{
                padding: '16px 32px',
                background: 'var(--color-success)',
                color: '#fff',
                border: 'none',
                borderRadius: '8px',
                fontSize: '1.2rem',
                fontWeight: 'bold',
                cursor: 'pointer',
                boxShadow: '0 4px 12px rgba(16, 185, 129, 0.3)'
              }}
            >
              View Live Leaderboard
            </button>
            {status === 'testing' && (
              <button 
                onClick={handleStopBenchmark}
                style={{
                  padding: '16px 32px',
                  background: 'var(--color-red)',
                  color: '#fff',
                  border: 'none',
                  borderRadius: '8px',
                  fontSize: '1.2rem',
                  fontWeight: 'bold',
                  cursor: 'pointer',
                  boxShadow: '0 4px 12px rgba(239, 68, 68, 0.3)'
                }}
              >
                Stop Benchmark
              </button>
            )}
          </div>
        )}

        {status === 'building' && !errorMsg && (
          <div style={{ marginTop: '30px', color: 'var(--color-amber)', padding: '16px', background: 'rgba(245, 158, 11, 0.1)', borderRadius: '8px' }}>
            ⚠️ <strong>Please do not close this page.</strong> Building the exchange container may take a few minutes.
          </div>
        )}

      </div>
    </div>
  )
}
