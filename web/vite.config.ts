import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// `npm run dev` serves the UI on port 5173 and passes /api to a running
// `code-search serve` on 7700.
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      "/api": {
        target: "http://127.0.0.1:7700",
        // The server only answers requests addressed to itself (Host), from
        // its own pages (Origin). Through this proxy both would name the dev
        // server, so they're set to the real server's.
        changeOrigin: true,
        headers: { origin: "http://127.0.0.1:7700" },
      },
    },
  },
  build: { outDir: "dist" },
});
