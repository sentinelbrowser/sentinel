         o   �     e���������<�p|C����2�@����b �            (�/� �5 D#[cfg(unix)]
mod ;

pub use self::::{
    IoVec,MAX_LENGTH,
}windows ip����%�01�����rJQ��y���     o     v  E     _�    ����F��4�I�Fѹ�fY<�r�S            (�/� �m �   ��
#[cfg(not(any(windows, unix)))]
mod unknown;
pub use self::::{
    IoVec,MAX_LENGTH,
};
 .B�*S�j�]�