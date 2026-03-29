# Requirement Summary

## Source
- Based on: Project_W26.pdf
- Course: COMP-8567
- Due date: Apr/13/2026

## 1. Project Objective
Build a socket-based client-server system where clients can query and retrieve files from a server-side directory tree rooted at home (`~`).
The system consists of three service programs:
- w26server (primary)
- mirror1 (copy of server)
- mirror2 (copy of server)

The client sends project-defined commands (not Linux commands) to request:
- directory listings
- single-file metadata
- compressed file bundles (`temp.tar.gz`) filtered by size/type/date

## 2. System Scope and Constraints
- Communication must use sockets only.
- `w26server`, `mirror1`, `mirror2`, and client must run on different machines/terminals.
- Server-side file search scope: directory tree rooted at server home (`~`).
- All files sent from server must be saved on client side under `~/project`.

## 3. Server Requirements (w26server / mirrors)
### 3.1 Startup and Listening
- `w26server`, `mirror1`, and `mirror2` must start before any client.
- Each server waits for client requests.

### 3.2 Per-Client Process Model
- On each client connection, server must `fork()` a child process.
- Child handles only that client in `crequest()`.
- Parent returns to listening for other clients.
- `crequest()` runs an infinite command-processing loop.
- On receiving `quitc`, `crequest()` exits.

### 3.3 Required Behavior Per Request
For each valid client command, server must:
- parse command parameters
- perform filesystem search/filter action
- return text response or `temp.tar.gz`
- return required not-found messages when no match exists

## 4. Client Requirements
### 4.1 Interactive Loop
- Client runs an infinite loop waiting for user input commands.

### 4.2 Command Validation
- Client must validate command syntax before sending to server.
- If syntax is invalid, client prints an appropriate error message.
- Only syntactically valid commands are sent to server.

### 4.3 Termination
- On `quitc`, client sends command to server and terminates.

## 5. Functional Requirements by Command
### 5.1 `dirlist -a`
- Server returns subdirectories (folders only) under server home.
- Sort order: alphabetical ascending.
- Client prints received list.

### 5.2 `dirlist -t`
- Server returns subdirectories under server home.
- Sort order: creation time ascending (oldest first).
- Client prints received list.

### 5.3 `fn filename`
- Server searches for `filename` in directory tree rooted at `~`.
- If found, server returns:
  - filename
  - size (bytes)
  - date created
  - file permissions
- If multiple matches exist, return first successful match only.
- If not found, client prints: `File not found`.

### 5.4 `fz size1 size2`
- Returns `temp.tar.gz` containing files with size in bytes where:
  - `size >= size1`
  - `size <= size2`
- Constraints: `size1 <= size2`, `size1 >= 0`, `size2 >= 0`.
- If no files match, return: `No file found`.

### 5.5 `ft <extension list>`
- Extension list includes 1 to 3 different file types.
- Returns `temp.tar.gz` containing files matching provided extensions.
- If no files match, return: `No file found`.

### 5.6 `fdb date`
- Returns `temp.tar.gz` containing files with creation date `<= date`.
- Example format shown: `YYYY-MM-DD`.

### 5.7 `fda date`
- Returns `temp.tar.gz` containing files with creation date `>= date`.
- Example format shown: `YYYY-MM-DD`.

### 5.8 `quitc`
- Command is sent to server.
- Client process terminates.

## 6. Connection Routing Requirement (Server/Mirror Alternation)
Connection handling order must be:
1. Connections 1-2 -> `w26server`
2. Connections 3-4 -> `mirror1`
3. Connections 5-6 -> `mirror2`
4. From connection 7 onward, alternate cyclically:
   - 7 -> `w26server`
   - 8 -> `mirror1`
   - 9 -> `mirror2`
   - ... continue repeating

## 7. Error Handling Requirements
- Invalid client command syntax: client prints appropriate error, no request processing.
- No match in `fn`: return/print `File not found`.
- No match in archive-producing commands (`fz`, `ft`, `fdb`, `fda`): return/print `No file found`.

## 8. Submission Requirements
Deliverables:
1. `w26server.c`
2. `client.c`
3. `mirror1.c`
4. `mirror2.c`

Additional constraints:
- Include adequate and relevant comments in code.
- Project demo and oral examination are required.
- Plagiarism checking via MOSS.

## 9. Acceptance Checklist (Practical)
- Three servers start and listen before any client.
- Each client connection is handled by a forked child process.
- All listed commands work with correct syntax validation.
- Correct routing sequence across `w26server`, `mirror1`, `mirror2` is observed.
- Returned archives are valid `temp.tar.gz` and saved under client `~/project`.
- Required not-found messages are produced exactly as specified.

## 10. Open Clarifications (to confirm with instructor)
- Exact definition of "date created" on Linux filesystem (creation time availability may vary).
- Exact wire protocol for transferring `temp.tar.gz` (size framing / end marker).
- Whether mirror routing is implemented by client-side selection or by a front-routing mechanism.
