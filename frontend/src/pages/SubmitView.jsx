import { useState, useCallback, useRef } from 'react'
import { useNavigate, Link } from 'react-router-dom'

export default function SubmitView() {
  const navigate = useNavigate()
  const [teamName, setTeamName] = useState('')
  const [email, setEmail] = useState('')
  const [language, setLanguage] = useState('')
  const [file, setFile] = useState(null)
  
  const [isDragging, setIsDragging] = useState(false)
  const [isUploading, setIsUploading] = useState(false)
  const [uploadProgress, setUploadProgress] = useState(0)
  const [errorMsg, setErrorMsg] = useState(null)
  
  const fileInputRef = useRef(null)

  const isTeamNameValid = teamName.length >= 3 && teamName.length <= 32 && /^[a-zA-Z0-9-]+$/.test(teamName)
  const isEmailValid = /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)
  const isLanguageValid = ['cpp', 'rust', 'go'].includes(language)
  const isFileValid = file && file.size <= 500 * 1024 * 1024

  const isFormValid = isTeamNameValid && isEmailValid && isLanguageValid && isFileValid

  const onDragEnter = useCallback((e) => {
    e.preventDefault()
    e.stopPropagation()
    setIsDragging(true)
  }, [])

  const onDragOver = useCallback((e) => {
    e.preventDefault()
    e.stopPropagation()
    setIsDragging(true)
  }, [])

  const onDragLeave = useCallback((e) => {
    e.preventDefault()
    e.stopPropagation()
    setIsDragging(false)
  }, [])

  const onDrop = useCallback((e) => {
    e.preventDefault()
    e.stopPropagation()
    setIsDragging(false)
    if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
      setFile(e.dataTransfer.files[0])
    }
  }, [])

  const onFileChange = (e) => {
    if (e.target.files && e.target.files.length > 0) {
      setFile(e.target.files[0])
    }
  }

  const formatSize = (bytes) => {
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB'
    return (bytes / (1024 * 1024)).toFixed(1) + ' MB'
  }

  const handleSubmit = () => {
    if (!isFormValid || isUploading) return
    setIsUploading(true)
    setErrorMsg(null)

    const formData = new FormData()
    formData.append('team_name', teamName)
    formData.append('email', email)
    formData.append('language', language)
    formData.append('file', file)

    const xhr = new XMLHttpRequest()
    xhr.open('POST', '/api/submit', true)

    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        const percent = Math.round((e.loaded / e.total) * 100)
        setUploadProgress(percent)
      }
    }

    xhr.onload = () => {
      setIsUploading(false)
      if (xhr.status === 200) {
        try {
          const res = JSON.parse(xhr.responseText);
          const submissionId = res.submission_id || (res.data && res.data.submission_id);
          if (submissionId) {
            navigate(`/submission/confirmation?id=${submissionId}&team=${encodeURIComponent(teamName)}`)
          } else {
            setErrorMsg("Submission succeeded but no ID returned.")
          }
        } catch (e) {
          setErrorMsg("Failed to parse server response.")
        }
      } else {
        try {
          const res = JSON.parse(xhr.responseText)
          setErrorMsg(res.error || `Upload failed with status ${xhr.status}`)
        } catch (e) {
          setErrorMsg(`Upload failed with status ${xhr.status}`)
        }
      }
    }

    xhr.onerror = () => {
      setIsUploading(false)
      setErrorMsg("Network error occurred during upload.")
    }

    xhr.send(formData)
  }

  return (
    <div className="app-container">
      <header className="header" style={{ justifyContent: 'flex-start', gap: '40px' }}>
        <Link to="/" style={{ color: 'var(--text-secondary)', textDecoration: 'none', fontSize: '1.2rem', padding: '10px' }}>
          &larr; Back
        </Link>
        <div className="header-brand">
          <div className="header-logo">⚡</div>
          <div>
            <h1 className="header-title">Submit Your Exchange</h1>
            <p className="header-subtitle">IICPC Summer Hackathon 2026</p>
          </div>
        </div>
      </header>

      <div style={{ maxWidth: '600px', margin: '40px auto', background: 'var(--bg-secondary)', padding: '32px', borderRadius: '12px', border: '1px solid var(--border)' }}>
        
        <div style={{ marginBottom: '24px' }}>
          <label style={{ display: 'block', marginBottom: '8px', fontWeight: 'bold' }}>Team Name</label>
          <input 
            type="text" 
            value={teamName} 
            onChange={e => setTeamName(e.target.value)} 
            placeholder="e.g. quantum-traders"
            style={{ width: '100%', padding: '12px', borderRadius: '6px', border: `1px solid ${teamName && !isTeamNameValid ? 'var(--color-red)' : 'var(--border)'}`, background: 'var(--bg-primary)', color: '#fff' }}
          />
          {teamName && !isTeamNameValid && <div style={{ color: 'var(--color-red)', fontSize: '0.85rem', marginTop: '4px' }}>3-32 characters, alphanumeric and hyphens only.</div>}
        </div>

        <div style={{ marginBottom: '24px' }}>
          <label style={{ display: 'block', marginBottom: '8px', fontWeight: 'bold' }}>Contact Email</label>
          <input 
            type="email" 
            value={email} 
            onChange={e => setEmail(e.target.value)} 
            placeholder="team@example.com"
            style={{ width: '100%', padding: '12px', borderRadius: '6px', border: `1px solid ${email && !isEmailValid ? 'var(--color-red)' : 'var(--border)'}`, background: 'var(--bg-primary)', color: '#fff' }}
          />
        </div>

        <div style={{ marginBottom: '24px' }}>
          <label style={{ display: 'block', marginBottom: '8px', fontWeight: 'bold' }}>Language</label>
          <select 
            value={language} 
            onChange={e => setLanguage(e.target.value)}
            style={{ width: '100%', padding: '12px', borderRadius: '6px', border: '1px solid var(--border)', background: 'var(--bg-primary)', color: '#fff' }}
          >
            <option value="" disabled>Select language...</option>
            <option value="cpp">C++</option>
            <option value="rust">Rust</option>
            <option value="go">Go</option>
          </select>
        </div>

        <div style={{ marginBottom: '32px' }}>
          <label style={{ display: 'block', marginBottom: '8px', fontWeight: 'bold' }}>Code Archive or Binary (Max 500MB)</label>
          <div 
            onDragEnter={onDragEnter}
            onDragOver={onDragOver}
            onDragLeave={onDragLeave}
            onDrop={onDrop}
            onClick={() => fileInputRef.current?.click()}
            style={{
              padding: '40px 20px',
              border: `2px dashed ${isDragging ? 'var(--color-primary)' : 'var(--border)'}`,
              borderRadius: '8px',
              textAlign: 'center',
              cursor: 'pointer',
              background: isDragging ? 'rgba(99, 102, 241, 0.1)' : 'var(--bg-primary)'
            }}
          >
            {file ? (
              <div>
                <div style={{ fontSize: '1.2rem', marginBottom: '8px' }}>📁 {file.name}</div>
                <div style={{ color: 'var(--text-muted)' }}>{formatSize(file.size)}</div>
              </div>
            ) : (
              <div style={{ color: 'var(--text-muted)' }}>
                Drag and drop your .zip or ELF binary here, or click to browse.
              </div>
            )}
            <input 
              type="file" 
              ref={fileInputRef} 
              style={{ display: 'none' }} 
              onChange={onFileChange} 
              accept=".zip,application/zip,application/x-executable,application/x-sharedlib,application/x-elf,application/octet-stream"
            />
          </div>
          {file && !isFileValid && <div style={{ color: 'var(--color-red)', fontSize: '0.85rem', marginTop: '8px' }}>File exceeds 500MB limit.</div>}
        </div>

        {isUploading && (
          <div style={{ marginBottom: '24px' }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: '8px', fontSize: '0.9rem' }}>
              <span>Uploading...</span>
              <span>{uploadProgress}%</span>
            </div>
            <div style={{ width: '100%', height: '8px', background: 'var(--bg-primary)', borderRadius: '4px', overflow: 'hidden' }}>
              <div style={{ width: `${uploadProgress}%`, height: '100%', background: 'var(--color-primary)', transition: 'width 0.2s' }} />
            </div>
          </div>
        )}

        {errorMsg && (
          <div style={{ marginBottom: '24px', padding: '16px', background: 'rgba(239, 68, 68, 0.1)', border: '1px solid var(--color-red)', borderRadius: '8px', color: 'var(--color-red)' }}>
            <strong>Upload Failed:</strong> {errorMsg}
          </div>
        )}

        <button 
          onClick={handleSubmit}
          disabled={!isFormValid || isUploading}
          style={{
            width: '100%',
            padding: '16px',
            background: (!isFormValid || isUploading) ? 'var(--bg-tertiary)' : 'var(--color-primary)',
            color: (!isFormValid || isUploading) ? 'var(--text-muted)' : '#fff',
            border: 'none',
            borderRadius: '8px',
            fontSize: '1.1rem',
            fontWeight: 'bold',
            cursor: (!isFormValid || isUploading) ? 'not-allowed' : 'pointer',
            transition: 'background 0.2s'
          }}
        >
          {isUploading ? 'Submitting...' : 'Submit Code'}
        </button>
      </div>

      <div style={{ maxWidth: '600px', margin: '0 auto', color: 'var(--text-secondary)' }}>
        <h3 style={{ color: 'var(--text-primary)', marginBottom: '16px' }}>Submission Expectations</h3>
        <ul style={{ lineHeight: '1.6', paddingLeft: '24px' }}>
          <li style={{ marginBottom: '8px' }}>Expose a REST or WebSocket endpoint for receiving bot fleet orders.</li>
          <li style={{ marginBottom: '8px' }}>Implement an order matching engine maintaining strict price-time priority.</li>
          <li>Handle massive concurrent bot connections efficiently without crashing or dropping packets.</li>
        </ul>
      </div>

    </div>
  )
}
