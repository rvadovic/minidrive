// @cubone/react-file-manager ships no type declarations; this covers the props this app uses
// (see the package README for the full list).
declare module "@cubone/react-file-manager" {
  import type { ComponentType } from "react";

  export interface CuboneFile {
    name: string;
    isDirectory: boolean;
    path: string;
    updatedAt?: string;
    size?: number;
  }

  export interface FileManagerProps {
    files: CuboneFile[];
    isLoading?: boolean;
    initialPath?: string;
    layout?: "list" | "grid";
    height?: string | number;
    width?: string | number;
    primaryColor?: string;
    fontFamily?: string;
    enableFilePreview?: boolean;
    collapsibleNav?: boolean;
    defaultNavExpanded?: boolean;
    permissions?: Partial<Record<"create" | "upload" | "move" | "copy" | "rename" | "download" | "delete", boolean>>;
    formatDate?: (date: string | Date) => string;
    onCreateFolder?: (name: string, parentFolder: CuboneFile | null) => void;
    onRename?: (file: CuboneFile, newName: string) => void;
    onDelete?: (files: CuboneFile[]) => void;
    onPaste?: (files: CuboneFile[], destinationFolder: CuboneFile | null, operationType: "copy" | "move") => void;
    onDownload?: (files: CuboneFile[]) => void;
    onRefresh?: () => void;
    onFolderChange?: (path: string) => void;
    onFileOpen?: (file: CuboneFile) => void;
    onSelectionChange?: (files: CuboneFile[]) => void;
    onError?: (error: { type: string; message: string }, file: CuboneFile) => void;
  }

  export const FileManager: ComponentType<FileManagerProps>;
}

declare module "@cubone/react-file-manager/dist/style.css";
