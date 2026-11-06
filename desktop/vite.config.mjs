import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// relative base so the built page loads from the app bundle; fixed port
// because tauri.conf.json points `devUrl` at it
export default defineConfig({
  base: "./",
  clearScreen: false,
  plugins: [react()],
  build: { outDir: "dist", emptyOutDir: true, target: ["es2021", "chrome105", "safari15"] },
  server: { port: 5173, strictPort: true },
});
