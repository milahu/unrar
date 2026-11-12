#include "rar.hpp"
#include "log.cpp"

// FIXME unix only
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

static MESSAGE_TYPE MsgStream=MSG_STDOUT;
static RAR_CHARSET RedirectCharset=RCH_DEFAULT;
static bool ProhibitInput=false;
static bool ConsoleOutputPresent=false;

static bool StdoutRedirected=false,StderrRedirected=false,StdinRedirected=false;

#ifdef _WIN_ALL
static bool IsRedirected(DWORD nStdHandle)
{
  HANDLE hStd=GetStdHandle(nStdHandle);
  DWORD Mode;
  return GetFileType(hStd)!=FILE_TYPE_CHAR || GetConsoleMode(hStd,&Mode)==0;
}
#endif


void InitConsole()
{
#ifdef _WIN_ALL
  // We want messages like file names or progress percent to be printed
  // immediately. Use only in Windows, in Unix they can cause wprintf %ls
  // to fail with non-English strings.
  setbuf(stdout,NULL);
  setbuf(stderr,NULL);

  // Detect if output is redirected and set output mode properly.
  // We do not want to send Unicode output to files and especially to pipes
  // like '|more', which cannot handle them correctly in Windows.
  // In Unix console output is UTF-8 and it is handled correctly
  // when redirecting, so no need to perform any adjustments.
  StdoutRedirected=IsRedirected(STD_OUTPUT_HANDLE);
  StderrRedirected=IsRedirected(STD_ERROR_HANDLE);
  StdinRedirected=IsRedirected(STD_INPUT_HANDLE);
#ifdef _MSC_VER
  if (!StdoutRedirected)
    _setmode(_fileno(stdout), _O_U16TEXT);
  if (!StderrRedirected)
    _setmode(_fileno(stderr), _O_U16TEXT);
#endif
#elif defined(_UNIX)
  StdoutRedirected=!isatty(fileno(stdout));
  StderrRedirected=!isatty(fileno(stderr));
  StdinRedirected=!isatty(fileno(stdin));
#endif
}


void SetConsoleMsgStream(MESSAGE_TYPE MsgStream)
{
  ::MsgStream=MsgStream;
}


void SetConsoleRedirectCharset(RAR_CHARSET RedirectCharset)
{
  ::RedirectCharset=RedirectCharset;
}


void ProhibitConsoleInput()
{
  ProhibitInput=true;
}


#ifndef SILENT
static void cvt_wprintf(FILE *dest,const wchar *fmt,va_list arglist)
{
  ConsoleOutputPresent=true;
  
  // No need for PrintfPrepareFmt here, vwstrprintf calls it.
  std::wstring s=vwstrprintf(fmt,arglist);

  // If format string contains \b, this is percent progress and we allow it.
  // But we reject \b too in arglist.
  if (wcschr(fmt,L'\b')==nullptr)
    ReplaceUnsafeChars(s);

#ifdef _WIN_ALL
  if (dest==stdout && StdoutRedirected || dest==stderr && StderrRedirected)
  {
    HANDLE hOut=GetStdHandle(dest==stdout ? STD_OUTPUT_HANDLE:STD_ERROR_HANDLE);
    DWORD Written;
    if (RedirectCharset==RCH_UNICODE)
      WriteFile(hOut,s.data(),(DWORD)s.size()*sizeof(s[0]),&Written,NULL);
    else
    {
      // Avoid Unicode for redirect in Windows, it does not work with pipes.
      std::string MsgA;
      if (RedirectCharset==RCH_UTF8)
        WideToUtf(s,MsgA);
      else
        WideToChar(s,MsgA);
      if (RedirectCharset==RCH_DEFAULT || RedirectCharset==RCH_OEM)
        CharToOemA(&MsgA[0],&MsgA[0]); // Console tools like 'more' expect OEM encoding.

      // We already converted \n to \r\n above, so we use WriteFile instead
      // of C library to avoid unnecessary additional conversion.
      WriteFile(hOut,MsgA.data(),(DWORD)MsgA.size(),&Written,NULL);
    }
    return;
  }
  // MSVC2008 vfwprintf writes every character to console separately
  // and it is too slow. We use direct WriteConsole call instead.
  HANDLE hOut=GetStdHandle(dest==stderr ? STD_ERROR_HANDLE:STD_OUTPUT_HANDLE);
  DWORD Written;
  WriteConsole(hOut,s.data(),(DWORD)s.size(),&Written,NULL);
#else
  fputws(s.c_str(),dest);
  // We do not use setbuf(NULL) in Unix (see comments in InitConsole).
  fflush(dest);
#endif
}


void mprintf(const wchar *fmt,...)
{
  if (MsgStream==MSG_NULL || MsgStream==MSG_ERRONLY)
    return;

  fflush(stderr); // Ensure proper message order.

  va_list arglist;
  va_start(arglist,fmt);
  FILE *dest=MsgStream==MSG_STDERR ? stderr:stdout;
  cvt_wprintf(dest,fmt,arglist);
  va_end(arglist);
}
#endif


#ifndef SILENT
void eprintf(const wchar *fmt,...)
{
  if (MsgStream==MSG_NULL)
    return;

  fflush(stdout); // Ensure proper message order.

  va_list arglist;
  va_start(arglist,fmt);
  cvt_wprintf(stderr,fmt,arglist);
  va_end(arglist);
}
#endif


#ifndef SILENT
static void QuitIfInputProhibited()
{
  // We cannot handle user prompts if -si is used to read file or archive data
  // from stdin.
  if (ProhibitInput)
  {
    mprintf(St(MStdinNoInput));
    ErrHandler.Exit(RARX_FATAL);
  }
}



#ifndef _WIN_ALL
#ifndef __VMS

// This is a POSIX implementation intended for Linux and other Unix-like systems.
// It deliberately forces termination on Ctrl+C,
// including when the previous SIGINT disposition was SIG_IGN.

static volatile sig_atomic_t PasswordInterrupted = 0;

static void PasswordSigInt(int)
{
  PasswordInterrupted = 1;
}

// Read password from /dev/tty without echo.
// Return false on error. Ctrl+C terminates via SIGINT.
static bool ReadPasswordTTY(char *Buf, size_t Size)
{
  if (Size == 0)
    return false;

  Buf[0] = 0;

  int Fd = open("/dev/tty", O_RDWR);
  if (Fd < 0)
    return false;

  struct termios OldTerm, NewTerm;
  if (tcgetattr(Fd, &OldTerm) != 0)
  {
    close(Fd);
    return false;
  }

  // Disable echo, enable signal processing and
  // canonical (line-based) input.
  NewTerm = OldTerm;
  NewTerm.c_lflag &= ~ECHO;
  NewTerm.c_lflag |= ISIG | ICANON;

  // Restore a usable Ctrl+C interrupt character
  // if the terminal has disabled it.
  if (NewTerm.c_cc[VINTR] == _POSIX_VDISABLE)
    NewTerm.c_cc[VINTR] = 3;

  struct sigaction OldAction, NewAction;
  memset(&NewAction, 0, sizeof(NewAction));
  NewAction.sa_handler = PasswordSigInt;
  sigemptyset(&NewAction.sa_mask);
  NewAction.sa_flags = 0; // No SA_RESTART.

  PasswordInterrupted = 0;

  if (sigaction(SIGINT, &NewAction, &OldAction) != 0)
  {
    close(Fd);
    return false;
  }

  bool TermChanged = false;
  bool Success = false;
  size_t Pos = 0;

  if (tcsetattr(Fd, TCSAFLUSH, &NewTerm) == 0)
  {
    TermChanged = true;

    // Read until newline, EOF, error or Ctrl+C.
    // Read one byte at a time to recognize literal
    // interrupt bytes even in unusual PTY setups.
    while (!PasswordInterrupted)
    {
      char Ch;
      ssize_t N = read(Fd, &Ch, 1);

      if (N < 0)
      {
        if (errno == EINTR)
        {
          if (PasswordInterrupted)
            break;
          continue;
        }
        break;
      }

      if (N == 0)
        break;

      // Fallback: Ctrl+C delivered as an input byte.
      // Normally ISIG causes SIGINT instead.
      if ((unsigned char)Ch ==
          (unsigned char)NewTerm.c_cc[VINTR])
      {
        PasswordInterrupted = 1;
        break;
      }

      // Also recognize literal ASCII Ctrl+C when
      // VINTR has been remapped.
      if ((unsigned char)Ch == 3)
      {
        PasswordInterrupted = 1;
        break;
      }

      if (Ch == '\n' || Ch == '\r')
      {
        Success = true;
        break;
      }

      // Do not truncate silently. Continue consuming
      // the line, but report failure if too long.
      if (Pos + 1 < Size)
        Buf[Pos++] = Ch;
      else
        Success = false;
    }
  }

  Buf[Pos] = 0;

  // Restore terminal settings before terminating.
  if (TermChanged)
    tcsetattr(Fd, TCSAFLUSH, &OldTerm);

  close(Fd);

  if (PasswordInterrupted)
  {
    // Erase the partial password.
    memset(Buf, 0, Size);

    // Restore normal SIGINT behavior, ensuring
    // Ctrl+C terminates even if previously ignored.
    struct sigaction DefaultAction;
    memset(&DefaultAction, 0, sizeof(DefaultAction));
    DefaultAction.sa_handler = SIG_DFL;
    sigemptyset(&DefaultAction.sa_mask);
    sigaction(SIGINT, &DefaultAction, NULL);

    // Ensure SIGINT is unblocked before raising it.
    sigset_t Set;
    sigemptyset(&Set);
    sigaddset(&Set, SIGINT);
    sigprocmask(SIG_UNBLOCK, &Set, NULL);

    raise(SIGINT);
    _exit(130);
  }

  sigaction(SIGINT, &OldAction, NULL);

  return Success;
}

#endif
#endif


#ifndef _WIN_ALL
#ifndef __VMS

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <errno.h>

static bool ReadPasswordTTY2(char *Buf, size_t Size)
{
  if (Size == 0)
    return false;

  Buf[0] = 0;

  // The descriptor is nonblocking, so a read() cannot become stuck if the input disappears between pselect() and read().
  int Fd = open("/dev/tty", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (Fd < 0)
    return false;

  struct termios OldTerm, NewTerm;
  if (tcgetattr(Fd, &OldTerm) != 0)
  {
    close(Fd);
    return false;
  }

  // Block SIGINT while modifying terminal state.
  sigset_t BlockSet, OldMask, WaitMask;
  sigemptyset(&BlockSet);
  sigaddset(&BlockSet, SIGINT);

  if (sigprocmask(SIG_BLOCK, &BlockSet, &OldMask) != 0)
  {
    close(Fd);
    return false;
  }

  // pselect temporarily restores the original mask,
  // allowing UnRAR's ProcessSignal() to run.
  WaitMask = OldMask;

  NewTerm = OldTerm;
  NewTerm.c_lflag &= ~(ECHO | ICANON);
  NewTerm.c_lflag |= ISIG;
  NewTerm.c_cc[VMIN] = 1;
  NewTerm.c_cc[VTIME] = 0;

  // Preserve the configured interrupt character.
  // Literal 0x03 is handled separately below.

  bool TermChanged = false;
  bool Success = false;
  bool Overflow = false;
  bool Interrupted = false;
  size_t Pos = 0;

  if (tcsetattr(Fd, TCSAFLUSH, &NewTerm) == 0)
  {
    TermChanged = true;

    while (!ErrHandler.UserBreak)
    {
      fd_set ReadSet;
      FD_ZERO(&ReadSet);
      FD_SET(Fd, &ReadSet);

      // Atomically wait for input while temporarily unblocking SIGINT.
      // pselect() atomically unblocks SIGINT while waiting. Once it returns, SIGINT is blocked again.
      int Ready = pselect(Fd + 1, &ReadSet, NULL, NULL,
                          NULL, &WaitMask);

      if (ErrHandler.UserBreak)
        break;

      if (Ready < 0)
      {
        if (errno == EINTR)
          continue;
        break;
      }

      // SIGINT is blocked again here.
      // O_NONBLOCK prevents a read() race.
      char Ch;
      ssize_t N = read(Fd, &Ch, 1);

      if (N < 0)
      {
        if (errno == EINTR || errno == EAGAIN)
          continue;
        break;
      }

      if (N == 0)
        break;

      unsigned char C = (unsigned char)Ch;

      // Some PTYs deliver Ctrl+C as literal 0x03
      // rather than generating SIGINT.
      if (C == 3 ||
          (NewTerm.c_cc[VINTR] != _POSIX_VDISABLE &&
           C == (unsigned char)NewTerm.c_cc[VINTR]))
      {
        Interrupted = true;
        break;
      }

      if (Ch == '\n' || Ch == '\r')
      {
        Success = !Overflow;
        break;
      }

      if (Pos + 1 < Size)
        Buf[Pos++] = Ch;
      else
        Overflow = true;
    }
  }

  if (ErrHandler.UserBreak)
    Interrupted = true;

  Buf[Pos] = 0;

  // SIGINT remains blocked during terminal restoration.
  bool Restored = true;
  if (TermChanged)
    Restored = tcsetattr(Fd, TCSAFLUSH, &OldTerm) == 0;

  close(Fd);

  if (Interrupted || !Restored)
  {
    // Clear the partial password.
    cleandata(Buf, Size);
    Success = false;
  }

  // Restore the original signal mask only after
  // terminal restoration.
  sigprocmask(SIG_SETMASK, &OldMask, NULL);

  if (Interrupted)
    ErrHandler.Exit(RARX_USERBREAK);

  return Success && Restored;
}

#endif
#endif




static void GetPasswordText(std::wstring &Str)
{
  QuitIfInputProhibited();
  if (StdinRedirected)
    getwstr(Str); // Read from pipe or redirected file.
  else
  {
#ifdef _WIN_ALL
    HANDLE hConIn=GetStdHandle(STD_INPUT_HANDLE);
    DWORD ConInMode;
    GetConsoleMode(hConIn,&ConInMode);
    SetConsoleMode(hConIn,ENABLE_LINE_INPUT); // Remove ENABLE_ECHO_INPUT.

    std::vector<wchar> Buf(MAXPASSWORD);
    
    // We prefer ReadConsole to ReadFile, so we can read Unicode input.
    DWORD Read=0;
    ReadConsole(hConIn,Buf.data(),(DWORD)Buf.size()-1,&Read,NULL);
    Buf[Read]=0;
    Str=Buf.data();
    cleandata(Buf.data(),Buf.size()*sizeof(Buf[0]));

    SetConsoleMode(hConIn,ConInMode);

    // 2023.03.12: Previously we checked for presence of "\n" in entered
    // passwords, supposing that truncated strings do not include it.
    // We did it to read the rest of excessively long string, so it is not
    // read later as the second password for -p switch. But this "\n" check
    // doesn't seem to work in Windows 10 anymore and "\r" is present even
    // in truncated strings. Also we increased MAXPASSWORD, so it is larger
    // than MAXPASSWORD_RAR. Thus we removed this check as not working
    // and not that necessary. Low level FlushConsoleInputBuffer doesn't help
    // for high level ReadConsole, which in line input mode seems to store
    // the rest of string in its own internal buffer.
#else
    std::vector<char> StrA(MAXPASSWORD*4); // "*4" for multibyte UTF-8 characters.
#ifdef __VMS
    fgets(StrA.data(),StrA.size()-1,stdin);
#elif defined(__sun)
    strncpyz(StrA.data(),getpassphrase(""),StrA.size());
#else
    #if false
    // ignore Ctrl+C on password prompt
    strncpyz(StrA.data(),getpass(""),StrA.size());
    #elif false
    // handle Ctrl+C on password prompt
    // no, this is not working
    // 2. Minimal source-code fix
    int tty = open("/dev/tty", O_RDONLY);
    if (tty >= 0)
    {
      struct termios original, modified;
      if (tcgetattr(tty, &original) == 0)
      {
        modified = original;
        modified.c_lflag |= ISIG;
        // Enable Ctrl+C interrupt processing.
        tcsetattr(tty, TCSANOW, &modified);
      }
      close(tty);
    }
    strncpyz(StrA.data(),getpass(""),StrA.size());
    #elif false
    // handle Ctrl+C on password prompt
    // yes, this works, "but it is not a fully race-free production implementation"
    // 3. Better fix: ensure the password reader preserves ISIG
    if (!ReadPasswordTTY(StrA.data(),StrA.size()))
    {
      cleandata(StrA.data(),StrA.size());
      ErrHandler.Exit(RARX_USERBREAK);
    }
    #elif true
    // handle Ctrl+C on password prompt
    // yes, this works
    if (!ReadPasswordTTY2(StrA.data(),StrA.size()))
    {
      cleandata(StrA.data(),StrA.size());
      ErrHandler.Exit(RARX_USERBREAK);
    }
    #endif
#endif
    CharToWide(StrA.data(),Str);
    cleandata(StrA.data(),StrA.size()*sizeof(StrA[0]));
#endif
  }
  RemoveLF(Str);
}
#endif


#ifndef SILENT
bool GetConsolePassword(UIPASSWORD_TYPE Type,const std::wstring &FileName,SecPassword *Password)
{
  if (!StdinRedirected)
    uiAlarm(UIALARM_QUESTION);
  
  while (true)
  {
//    if (!StdinRedirected)
      if (Type==UIPASSWORD_GLOBAL)
        eprintf(L"\n%s: ",St(MAskPsw));
      else
        eprintf(St(MAskPswFor),FileName.c_str());

    std::wstring PlainPsw;
    GetPasswordText(PlainPsw);
    if (PlainPsw.empty() && Type==UIPASSWORD_GLOBAL)
      return false;
    if (PlainPsw.size()>=MAXPASSWORD)
    {
      PlainPsw.erase(MAXPASSWORD-1);
      uiMsg(UIERROR_TRUNCPSW,MAXPASSWORD-1);
    }
    if (!StdinRedirected && Type==UIPASSWORD_GLOBAL)
    {
      eprintf(St(MReAskPsw));
      std::wstring CmpStr;
      GetPasswordText(CmpStr);
      if (CmpStr.empty() || PlainPsw!=CmpStr)
      {
        eprintf(St(MNotMatchPsw));
        cleandata(&PlainPsw[0],PlainPsw.size()*sizeof(PlainPsw[0]));
        cleandata(&CmpStr[0],CmpStr.size()*sizeof(CmpStr[0]));
        continue;
      }
      cleandata(&CmpStr[0],CmpStr.size()*sizeof(CmpStr[0]));
    }
    Password->Set(PlainPsw.c_str());
    cleandata(&PlainPsw[0],PlainPsw.size()*sizeof(PlainPsw[0]));
    break;
  }
  return true;
}
#endif


#ifndef SILENT
void getwstr(std::wstring &str)
{
  // Print buffered prompt title function before waiting for input.
  fflush(stderr);

  QuitIfInputProhibited();

  str.clear();

  const size_t MaxRead=MAXPATHSIZE; // Large enough to read a file name.

#if defined(_WIN_ALL)
  // fgetws does not work well with non-English text in Windows,
  // so we do not use it.
  if (StdinRedirected) // ReadConsole does not work if redirected.
  {
    // fgets does not work well with pipes in Windows in our test.
    // Let's use files.
    std::vector<char> StrA(MaxRead*4);  // Up to 4 UTF-8 characters per wchar_t.
    File SrcFile;
    SrcFile.SetHandleType(FILE_HANDLESTD);
    SrcFile.SetLineInputMode(true);
    int ReadSize=SrcFile.Read(&StrA[0],StrA.size()-1);
    if (ReadSize<=0)
    {
      // Looks like stdin is a null device. We can enter to infinite loop
      // calling Ask() or set an empty password, so let's better exit.
     ErrHandler.ReadError(L"stdin");
    }
    StrA[ReadSize]=0;

    // We expect ANSI encoding here, but "echo text|rar ..." to pipe to RAR,
    // such as send passwords, we get OEM encoding by default, unless we
    // use "chcp" in console. But we avoid OEM to ANSI conversion,
    // because we also want to handle ANSI files redirection correctly,
    // like "rar ... < ansifile.txt".
    CharToWide(&StrA[0],str);
    cleandata(&StrA[0],StrA.size()); // We can use this function to enter passwords.
  }
  else
  {
    std::vector<wchar> Buf(MaxRead);  // Up to 4 UTF-8 characters per wchar_t.
    DWORD SizeToRead=(DWORD)Buf.size()-1;

    // ReadConsole fails in Windows 7 for requested input exceeding 30 KB.
    // Not certain about Windows 8, so check for Windows 10 here.
    if (WinNT()<=WNT_W10)
      SizeToRead=Min(SizeToRead,0x4000);

    DWORD ReadSize=0;
    if (ReadConsole(GetStdHandle(STD_INPUT_HANDLE),&Buf[0],SizeToRead,&ReadSize,NULL)==0)
      ErrHandler.ReadError(L"stdin"); // Unknown user input, safer to abort.
    Buf[ReadSize]=0;
    str=Buf.data();
  }
#else
  std::vector<wchar> Buf(MaxRead);  // Up to 4 UTF-8 characters per wchar_t.
  if (fgetws(&Buf[0],Buf.size(),stdin)==NULL)
    ErrHandler.ReadError(L"stdin"); // Avoid infinite Ask() loop.
  str=Buf.data();
#endif
  RemoveLF(str);
}
#endif


#ifndef SILENT
// We allow this function to return 0 in case of invalid input,
// because it might be convenient to press Enter to some not dangerous
// prompts like "insert disk with next volume". We should call this function
// again in case of 0 in dangerous prompt such as overwriting file.
int Ask(const std::vector<std::wstring> &Items)
{
  uiAlarm(UIALARM_QUESTION);

  std::vector<uint> ItemsHotPos; // Hotkey positions for request words.

  for (size_t I=0;I<Items.size();I++)
  {
    const std::wstring &CurItem=Items[I];

    uint HotPos=0; // Hotkey position in current word.
    for (;HotPos<CurItem.size();HotPos++)
    {
      wchar CurKey=toupperw(CurItem[HotPos]); // Character to try as hotkey.

      // Check if current character of current word was already used
      // as a hotkey in preceding words.
      bool AlreadyUsed=false;
      for (uint J=0;J<I && !AlreadyUsed;J++)
        if (toupperw(Items[J][ItemsHotPos[J]])==CurKey)
          AlreadyUsed=true;

      // Search until we find an unused character except space, which isn't
      // suitable for hotkey, or until all characters are verified.
      if (!AlreadyUsed && CurKey!=' ')
        break;
    }
    // Store either the hotkey position or position of trailing 0 if no hotkeys
    // for current word is available.
    ItemsHotPos.push_back(HotPos);
  }

  uint NumericHotkey=1; // Current numeric hotkey if all alphabetical are already used.

  for (uint I=0;I<Items.size();I++)
  {
    eprintf(I==0 ? (Items.size()>3 ? L"\n":L" "):L", ");
    uint HotPos=ItemsHotPos[I];
    std::wstring CurStr=Items[I];
    wcslower(CurStr); // In case a localized version has uppercase characters.
    std::wstring Prefix=CurStr.substr(0,HotPos);
    eprintf(L"%s",Prefix.c_str()); // Print the part of string before hotkey.

    wchar HotChar=CurStr[HotPos];
    if (HotChar!=0)
      eprintf(L"[%c]%ls",toupperw(HotChar),&CurStr[HotPos+1]); // Print hotkey and rest of word.
    else
      if (NumericHotkey<10)
        eprintf(L"[%c]",'0'+NumericHotkey++); // If no hotkey is available for current word, assign a numeric one.
  }
  eprintf(L" ");
  std::wstring Str;
  getwstr(Str);
  wchar Ch=toupperw(Str[0]);

  NumericHotkey=1;
  for (uint I=0;I<Items.size();I++)
  {
    wchar HotChar=Items[I][ItemsHotPos[I]];
    if (HotChar==0 && NumericHotkey<10)
    {
      if (Ch=='0'+NumericHotkey) // If no hotkey is available for current word, check a numeric one.
        return I+1;
      NumericHotkey++; // Go to next numeric hotkey.
    }
    else
      if (Ch==toupperw(HotChar))
        return I+1;
  }
  return 0; // No hotkeys matched.
}
#endif


void OutComment(const std::wstring &Comment)
{
  const size_t MaxOutSize=0x400;
  for (size_t I=0;I<Comment.size();I+=MaxOutSize)
  {
    size_t CopySize=Min(MaxOutSize,Comment.size()-I);
    mprintf(L"%s",Comment.substr(I,CopySize).c_str());
  }
  mprintf(L"\n");
}


bool IsConsoleOutputPresent()
{
  return ConsoleOutputPresent;
}
