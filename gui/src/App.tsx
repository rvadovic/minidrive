import { ErrorBoundary } from "./components/ErrorBoundary";
import { FeedbackProvider } from "./components/Feedback";
import { ConnectScreen } from "./components/ConnectScreen";
import { MainView } from "./components/MainView";
import { QuestionDialog } from "./components/QuestionDialog";
import { ProfilesProvider } from "./profiles";
import { SessionProvider, useSession } from "./session";

function Shell() {
  const { phase } = useSession();
  return (
    <>
      {phase === "ready" ? <MainView /> : <ConnectScreen />}
      <QuestionDialog />
    </>
  );
}

export function App() {
  return (
    <ErrorBoundary>
    <FeedbackProvider>
      <ProfilesProvider>
        <SessionProvider>
          <Shell />
        </SessionProvider>
      </ProfilesProvider>
    </FeedbackProvider>
    </ErrorBoundary>
  );
}
