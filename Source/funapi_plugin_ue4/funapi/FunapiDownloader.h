﻿// Copyright (C) 2013-2015 iFunFactory Inc. All Rights Reserved.
//
// This work is confidential and proprietary to iFunFactory Inc. and
// must not be used, disclosed, copied, or distributed without the prior
// consent of iFunFactory Inc.

#pragma once


namespace Fun
{
    class AsyncThread;

    class FunapiDownloader
    {
    public:
        FunapiDownloader();
        ~FunapiDownloader();

    private:
        AsyncThread* Thread;
    };

} // namespace Fun
