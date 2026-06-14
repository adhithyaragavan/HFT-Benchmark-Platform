import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    port: 3000,
    proxy: {
      '/api/v1/leaderboard': {
        target: 'http://localhost:8093',
        ws: true,
        changeOrigin: true,
      },
      '/api': {
        target: 'http://localhost:8090',
        changeOrigin: true,
      },
      '/ws': {
        target: 'ws://localhost:8093',
        ws: true,
      },
    },
  },
})
