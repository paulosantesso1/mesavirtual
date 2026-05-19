# Mesa de Som Mix Virtual v3.0

Ferramenta para Windows que captura o áudio de programas específicos e de um microfone, mistura tudo e envia para uma placa de áudio virtual — como o VB-Cable — para transmissão em aplicativos como TeamTalk, Discord ou OBS.

O que outros programas não permitem, como escolher quais aplicativos entram na transmissão e quais ficam só no seu fone, este projeto resolve.

---

## Como funciona

O projeto é composto por dois executáveis que trabalham juntos:

**Placasom.exe** é o motor de áudio. Ele roda em segundo plano, sem janela, e faz o trabalho pesado: captura o microfone físico, captura o áudio de cada programa marcado individualmente (usando a API de Process Loopback do Windows 11), mistura tudo e escreve na placa virtual e no fone ao mesmo tempo.

**PlacaGui.exe** é a interface gráfica. Você escolhe os dispositivos, marca os programas que quer transmitir e aperta um botão. Ela inicia o motor com os parâmetros certos e fica recebendo os ajustes de volume que você faz nos controles.

O fluxo de áudio é este:

```
Microfone físico ──────────────────────────────────────┐
                                                       ├──► Cabo virtual (transmissão)
Programas marcados (TeamTalk, Chrome, etc.) ────────────┘
                                                       └──► Fone de ouvido (só o microfone)
Programas NÃO marcados ──► continuam indo para o fone normalmente pelo Windows
```

---

## Requisitos

- Windows 11 (a API de Process Loopback exige Windows 11 ou Windows 10 versão 20348 ou superior)
- Uma placa de áudio virtual instalada, como o [VB-Cable](https://vb-audio.com/Cable/) (gratuito)
- Visual Studio 2022 com as cargas de trabalho **Desenvolvimento para Desktop com C++** e **Desenvolvimento para Desktop com .NET**

---

## Compilando

### 1. AudioEngine (o motor — C++)

Abra o Prompt de Comando do Desenvolvedor do Visual Studio (procure por "Developer Command Prompt" no menu Iniciar) e execute:

```
cl /std:c++17 /O2 /EHsc AudioEngine.cpp /Fe:Placasom.exe Ole32.lib
```

Isso gera o arquivo `Placasom.exe`.

Se preferir usar o Visual Studio com interface gráfica, crie um projeto do tipo **Console Application (C++)**, adicione o `AudioEngine.cpp`, e nas propriedades do projeto defina:

- C++ Language Standard: C++17 ou superior
- Em Linker > Input > Additional Dependencies, adicione: `Ole32.lib`

### 2. PlacaGui (a interface — C#)

Abra o Visual Studio 2022 e crie um novo projeto do tipo **Windows Forms App (.NET)**, com framework **.NET 6** ou superior.

Substitua o `Form1.cs` gerado pelo arquivo `Form1.cs` deste projeto. Depois compile normalmente com Ctrl+Shift+B ou pelo menu Build > Build Solution.

### 3. Colocando tudo junto

Após compilar os dois, coloque o `Placasom.exe` na mesma pasta que o `PlacaGui.exe`. A interface busca o motor nessa mesma pasta ao iniciar.

---

## Como usar

1. Instale o VB-Cable (ou outra placa virtual de sua preferência) e reinicie o computador se pedido.

2. Abra o `PlacaGui.exe`.

3. No campo **Microfone físico**, selecione seu microfone. Se não quiser transmitir sua voz, selecione "Desativado".

4. No campo **Cabo virtual de saída**, selecione o VB-Cable Input (ou equivalente). É por aqui que o áudio vai para o TeamTalk, Discord ou OBS.

5. No campo **Retorno**, selecione seu fone de ouvido. Este campo é opcional — é para você ouvir sua própria voz enquanto fala. Se não quiser esse retorno, selecione "Nenhum".

6. Na lista de **Programas**, marque com Espaço ou clique nos que você quer que o áudio vá para a transmissão. Os desmarcados continuam tocando normalmente no seu fone, mas não entram na transmissão.

7. Clique em **Iniciar Transmissão**.

8. No TeamTalk (ou Discord, OBS, etc.), selecione o VB-Cable como dispositivo de entrada de microfone.

Para ajustar os volumes sem parar a transmissão, abra **Configurações**. Os sliders de volume do microfone e dos programas atualizam em tempo real enquanto a transmissão está ativa.

---

## Configurações disponíveis

Acessadas pelo botão "Configurações" na tela principal.

**Volume do microfone** — controla o quanto da sua voz entra na transmissão. 100% é o nível original. Valores acima de 100% amplificam.

**Volume da transmissão** — controla o quanto do áudio dos programas marcados entra na transmissão. Funciona da mesma forma.

**Minimizar para a bandeja** — quando ativado, minimizar a janela a coloca na área de notificação (perto do relógio), sem aparecer na barra de tarefas. Clique duplo no ícone para reabrir.

As configurações são salvas automaticamente no Registro do Windows e restauradas na próxima vez que abrir o programa.

---

## Detalhes técnicos

Esta seção é opcional — explica as decisões internas para quem quiser entender ou modificar o projeto.

**Por que Process Loopback e não o loopback geral do Windows?**
O loopback geral captura todo o áudio do sistema. Se você usasse ele, o áudio do cabo virtual entraria na captura e criaria um eco em cascata infinito. O Process Loopback permite capturar cada programa individualmente, usando o modo `INCLUDE_TARGET_PROCESS_TREE`, que pega apenas aquele processo e seus filhos — cobrindo casos como o Chrome, que roda o áudio num processo separado.

**Por que o modo precisa ser INCLUDE e não EXCLUDE?**
`EXCLUDE_TARGET_PROCESS_TREE` captura tudo do sistema *exceto* o processo indicado, que é o oposto do que se quer. Com EXCLUDE, marcar o TeamTalk faria capturar todos os outros programas menos o TeamTalk — gerando eco de tudo. Com `INCLUDE`, captura-se apenas o processo marcado.

**Como o volume em tempo real funciona sem reiniciar?**
Os volumes internos do motor são `std::atomic<float>` — um tipo de variável que pode ser lida e escrita de threads diferentes ao mesmo tempo sem travar o processamento. A interface envia comandos de texto (`mic:80`, `proc:120`) para a entrada padrão do processo motor, que os lê e atualiza os atômicos na hora, sem interromper a captura ou a mixagem.

**O que é o LockFreeRingBuffer?**
É uma fila circular sem travas (lock-free) do tipo SPSC — um produtor, um consumidor. Cada thread de captura escreve amostras de áudio nela, e a thread do mixer lê. Sem travas porque as threads de captura e de mixagem nunca leem e escrevem no mesmo índice ao mesmo tempo, e os índices são atualizados com operações atômicas de memória.

**Por que o fone recebe apenas o microfone e não os programas?**
Por escolha de design: os programas já estão tocando normalmente no fone pelo Windows. Se o motor enviasse o áudio dos programas para o fone também, você ouviria tudo em dobro.

---

## Problemas comuns

**O áudio não chega no TeamTalk.** Confirme que no TeamTalk o dispositivo de entrada de microfone está selecionado como VB-Cable Output (não Input). O VB-Cable tem dois lados: o Input é onde o motor escreve, e o Output é onde outros programas lêem.

**Aparece eco ou som de todos os programas mesmo sem marcá-los.** Isso indica que o modo de loopback está como EXCLUDE em vez de INCLUDE no `AudioEngine.cpp`. Verifique a linha que define `ProcessLoopbackMode`.

**A lista de programas está vazia.** Clique em "Atualizar Lista". A lista mostra apenas programas com janela aberta, navegadores e leitores de tela. Programas rodando em segundo plano sem janela não aparecem.

**O programa não inicia a transmissão e diz que o motor não foi encontrado.** Confirme que o `Placasom.exe` está na mesma pasta que o `PlacaGui.exe`.
