// pch.h — o cabeçalho pré-compilado.
//
// Trazer o C++/WinRT inteiro para cá é o que faz o build do aplicativo não demorar minutos por
// arquivo: as projeções do WinRT são pesadas e são as mesmas em todos os `.cpp`.
#pragma once

// **`NOMINMAX` antes de qualquer header do Windows.** O `<windows.h>` define `min` e `max` como
// macros; sem esta linha um `std::max` vira `std::(...)` e o compilador acusa
// `C2589: '(': illegal token on right side of '::'` — um erro que não aponta para a macro que o
// causou. O `CreateFile2` do `Renderer.cpp` é o outro motivo de o Windows vir por aqui.
#define NOMINMAX

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
// **O `Xaml.Markup.h` não é opcional.** É dele que vem a especialização de
// `winrt::impl::produce<D, IComponentConnector>` de que o `MainPageT` gerado precisa. Sem ele o
// build para em `C2079: 'winrt::impl::producer<...IComponentConnector...>::vtable' uses undefined
// struct 'winrt::impl::produce<D,I>'` — um erro que cai dentro do `base.h` do próprio SDK e não
// sugere o include que falta. Medido: com a linha, o erro some; sem ela, aparece duas vezes (uma
// por `IComponentConnector`, outra por `IComponentConnector2`). O `Data.h` é o do `{Binding}` do
// `MainPage.xaml`, e o `Interop.h` é o par dele.
#include <winrt/Windows.UI.Xaml.Data.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Navigation.h>

// O `ISwapChainPanelNative` não tem projeção: é uma interface clássica do XAML, e é por ela que
// o `IDXGISwapChain` que criamos chega ao painel.
#include <windows.ui.xaml.media.dxinterop.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>
