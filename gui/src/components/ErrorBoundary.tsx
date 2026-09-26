import { Component, type ErrorInfo, type ReactNode } from "react";
import { api } from "../api";

// A rendering bug must not leave the user looking at an empty window with a live session behind
// it: show what broke, and offer the way back.
export class ErrorBoundary extends Component<{ children: ReactNode }, { error: Error | null }> {
  state = { error: null as Error | null };

  static getDerivedStateFromError(error: Error) {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error("MiniDrive UI error", error, info.componentStack);
  }

  render() {
    if (!this.state.error) return this.props.children;
    return (
      <div className="panel-view" role="alert" data-testid="ui-error">
        <h2>Something went wrong in the MiniDrive window</h2>
        <p className="callout callout-error">{String(this.state.error.message || this.state.error)}</p>
        <pre className="hint">{this.state.error.stack}</pre>
        <button
          className="btn btn-primary"
          onClick={() => {
            // The session lives in the Rust core, not in this page; end it so the reloaded window
            // starts clean instead of finding itself "already connected".
            void api.disconnect().catch(() => null).finally(() => location.reload());
          }}
        >
          Disconnect and reload
        </button>
      </div>
    );
  }
}
