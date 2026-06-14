import { BrowserRouter, Routes, Route } from 'react-router-dom'
import LeaderboardView from './pages/LeaderboardView'
import RunDetailView from './pages/RunDetailView'
import SubmitView from './pages/SubmitView'
import SubmissionConfirmationView from './pages/SubmissionConfirmationView'

export default function App() {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/" element={<LeaderboardView />} />
        <Route path="/run/:runId" element={<RunDetailView />} />
        <Route path="/submit" element={<SubmitView />} />
        <Route path="/submission/confirmation" element={<SubmissionConfirmationView />} />
      </Routes>
    </BrowserRouter>
  )
}
