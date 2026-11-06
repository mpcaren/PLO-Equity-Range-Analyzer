import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// relative base so the built page loads from file:// inside Electron
export default defineConfig({
  base: "./",
  plugins: [react()],
  build: { outDir: "dist", emptyOutDir: true, target: "chrome130" },
  server: { port: 5173, strictPort: false },
});
