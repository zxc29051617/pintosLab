FROM ubuntu:22.04

RUN apt update && apt install -y --no-install-recommends \
    ca-certificates \
    build-essential \
    gdb \
    qemu-system-x86 \
    nasm \
    vim \
    perl \
    python3 \
    python3-pip \
    file \
    git \
    texinfo \
    unzip \
    wget && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/hugsy/gef.git /usr/local/share/gef && \
    ln -s /usr/local/share/gef/gef.py /usr/local/bin/gef.py

WORKDIR /home
RUN wget https://github.com/Xueyi-Chen-David/pintos/archive/refs/heads/main.zip --no-check-certificate && \
    unzip main.zip && \
    mv pintos-main pintos && \
    rm main.zip

WORKDIR /home/pintos/utils
RUN chmod -R +x /home/pintos/utils
RUN mv /home/pintos/utils/* /usr/local/bin/
RUN ln -s $(which qemu-system-x86_64) /usr/local/bin/qemu

RUN rm -r /home/pintos

RUN echo 'source /usr/local/bin/gef.py' >> /root/.gdbinit

RUN echo "export LC_ALL=C.UTF-8" >> /root/.bashrc && \
    echo "export LANG=C.UTF-8" >> /root/.bashrc

WORKDIR /home

CMD ["sleep", "infinity"]
