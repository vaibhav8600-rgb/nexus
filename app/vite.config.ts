import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

// base './' so the same build works at a GitHub Pages sub-path and at a root.
export default defineConfig({
  base: './',
  plugins: [react()],
  test: { environment: 'node' },
});
