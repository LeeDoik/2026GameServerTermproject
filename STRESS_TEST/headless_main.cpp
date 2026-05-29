// 헤드리스 부하 생성기 — 기존 STRESS_TEST/NetworkModule.cpp의 네트워크 로직을 그대로 사용.
// DrawModule(OpenGL/Win32 창)을 빼고 콘솔로만 구동해 자동/원격 환경에서도 안전하게 측정한다.
// NetworkModule.cpp가 참조하는 전역 hWnd만 스텁으로 제공한다.
#include <winsock2.h>
#include <windows.h>

HWND hWnd = NULL;  // NetworkModule.cpp의 `extern HWND hWnd;`를 만족시키는 스텁

void InitializeNetwork();  // NetworkModule.cpp에 정의 — 워커/테스트 스레드 기동 + cin으로 IP 입력

int main() {
    InitializeNetwork();   // 내부에서 IP를 stdin으로 읽고 Test/Worker 스레드를 무한 구동
    Sleep(INFINITE);       // 부모 프로세스가 측정 시간 후 종료시킴. [Stress] 로그는 cout으로 출력됨
    return 0;
}
