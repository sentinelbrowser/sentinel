         �   �     
�(���������T7���8��X�H�(�.�            u@echo off
setlocal
:: This is required with cygwin only.
PATH=%~dp0;%PATH%
set PYTHONDONTWRITEBYTECODE=1
call python "%~dp0mb.py" %*
     �     *   �     
�&    ����Y���7(����]�m��7i�               i   �   call vpython3 "%~dp0mb.py" %*
