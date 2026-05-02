import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.Closeable;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.rmi.Remote;
import java.util.HashMap;
import java.util.Map;

public final class RemoteFileDemo {
    private static final Map<Integer, RemoteTextFile> REMOTE_FILES = new HashMap<Integer, RemoteTextFile>();
    private static int nextId = 0;

    private RemoteFileDemo() {
    }

    private static final class RemoteTextFile implements Remote, Closeable {
        private final BufferedWriter writer;

        private RemoteTextFile(String path) throws IOException {
            this.writer = new BufferedWriter(new FileWriter(path, true));
        }

        private void writeLine(String text) throws IOException {
            writer.write(text);
            writer.newLine();
            writer.flush();
        }

        public void close() throws IOException {
            writer.close();
        }
    }

    public static void main(String[] args) throws Exception {
        String outputPath = args.length > 0 ? args[0] : "build/lab4_remote_file_demo.txt";

        if (args.length > 1 && "--interactive".equals(args[1])) {
            runInteractive(outputPath);
            return;
        }

        int ref = openRemoteFile(outputPath);
        writeByRef(ref, "first line: written through saved Remote object");

        System.gc();
        Thread.sleep(100);

        writeByRef(ref, "second line: file is still alive after System.gc()");
        closeByRef(ref);

        System.out.println("Remote file ref: @" + ref);
        System.out.println("Wrote demo lines to: " + outputPath);
    }

    private static void runInteractive(String outputPath) throws Exception {
        BufferedReader input = new BufferedReader(new InputStreamReader(System.in, "UTF-8"));
        int ref = openRemoteFile(outputPath);
        String line;

        System.out.println("Remote file ref: @" + ref);
        System.out.println("Writing to: " + outputPath);
        System.out.println("Type text and press Enter. Type quit to close the file.");

        while ((line = input.readLine()) != null) {
            if ("quit".equals(line)) {
                closeByRef(ref);
                System.out.println("closed @" + ref);
                return;
            }
            writeByRef(ref, line);
            System.out.println("written @" + ref);
        }

        closeByRef(ref);
    }

    private static int openRemoteFile(String path) throws IOException {
        int id = nextId++;
        REMOTE_FILES.put(Integer.valueOf(id), new RemoteTextFile(path));
        return id;
    }

    private static void writeByRef(int id, String text) throws IOException {
        RemoteTextFile file = getRemoteFile(id);
        file.writeLine(text);
    }

    private static void closeByRef(int id) throws IOException {
        RemoteTextFile file = REMOTE_FILES.remove(Integer.valueOf(id));
        if (file != null) {
            file.close();
        }
    }

    private static RemoteTextFile getRemoteFile(int id) {
        RemoteTextFile file = REMOTE_FILES.get(Integer.valueOf(id));
        if (file == null) {
            throw new IllegalArgumentException("unknown remote file ref: @" + id);
        }
        return file;
    }
}
